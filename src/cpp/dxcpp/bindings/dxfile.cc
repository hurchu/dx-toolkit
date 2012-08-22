#include <vector>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp> //include all types plus i/o
#include "dxfile.h"
#include "SimpleHttp.h"

using namespace std;
using namespace dx;

const int64_t DXFile::max_buf_size_ = 104857600;

// A helper function for making http requests with retry logic
void makeHTTPRequestForFileReadAndWrite(HttpRequest &resp, const string &url, const HttpHeaders &headers, const HttpMethod &method, const char *data = NULL, const size_t size=0u) {
  const int MAX_TRIES = 5;
  int retries = 0;
  bool someThingWentWrong = false;
  string wrongThingDescription = "";
  while (true) { 
    try {
      resp = HttpRequest::request(method, url, headers, data, size);
    } catch(HttpRequestException e) {
      someThingWentWrong = true;
      wrongThingDescription = e.what();
    }
   
    if (!someThingWentWrong && (resp.responseCode < 200 || resp.responseCode >= 300)) {
      someThingWentWrong = true;
      wrongThingDescription = "Server returned HTTP Response code = " + boost::lexical_cast<string>(resp.responseCode);
    }
/*    if (!someThingWentWrong && resp.respData.size() == 0) {
      someThingWentWrong = true;
      wrongThingDescription = "Server returned HTTP response code =  " + boost::lexical_cast<string>(resp.responseCode) + ". But response size = 0 (unexpected)";
    }*/

    if (someThingWentWrong) {
      retries++;
      if (retries >= MAX_TRIES) {
        vector<string> hvec = headers.getAllHeadersAsVector();
        string headerStr = "HTTP Headers sent with request:";
        headerStr += (hvec.size() == 0) ? " None\n" : "\n";
        for (int i = 0; i < hvec.size(); ++i) {
          headerStr += "\t" + boost::lexical_cast<string>(i + 1) + ")" +  hvec[i] + "\n";
        }
        throw DXFileError(string("******\nERROR (Unrecoverable): while performing : '") + getHttpMethodName(method) + " " + url + "'" + ".\n" + headerStr + "Giving up after " + boost::lexical_cast<string>(retries) + " tries.\nError message: " + wrongThingDescription + "\n******\n");
      }
      
      // TODO: Make printing to stderr thread safe someday ?
      //       Though we are writing data to std::cerr in a single call to operator <<()
      //       (rather than chaining <<). It is not clear if a single call is thread safe.
      //       C++03 certainly did not provide any thread safe guarantees (it didn't
      //       even recognize that "threads" exist at all!). 
      //       Not sure if C++11 provides a thread safe guarantee for call to operator<<() 
      //       (including the flush in case of std::cerr).
      //       Anyway, *observed* behavior (when compiled in g++ 4.6.3) is that output is *NOT*
      //       garbled, and work as if <<() was a thread safe call. :)
      std::cerr<<("\nRetry #" + boost::lexical_cast<string>(retries) + ": Will start retrying '" + getHttpMethodName(method) + " " + url + "' in " + boost::lexical_cast<string>(1<<retries) + " seconds. Error in previous try: " + wrongThingDescription);
      usleep((1<<retries) * 1000 * 1000);
      someThingWentWrong = false;
      wrongThingDescription.clear();
      continue; // repeat the same request
    }
    return;
  }
}

void DXFile::init_internals_() {
  pos_ = 0;
  file_length_ = -1;
  buffer_.str(string());
  buffer_.clear();
  cur_part_ = 1;
  eof_ = false;
  is_closed_ = false;
  countThreadsWaitingOnConsume = 0;
  countThreadsNotWaitingOnConsume = 0; 
}

void DXFile::setIDs(const string &dxid, const string &proj) {
  stopLinearQuery();
  flush();
  init_internals_();
  DXDataObject::setIDs(dxid, proj);
}

void DXFile::create(const std::string &media_type,
		    const dx::JSON &data_obj_fields) {
  JSON input_params = data_obj_fields;
  if (!data_obj_fields.has("project"))
    input_params["project"] = g_WORKSPACE_ID;
  if (media_type != "")
    input_params["media"] = media_type;
  const JSON resp = fileNew(input_params);

  setIDs(resp["id"].get<string>(), input_params["project"].get<string>());
}

void DXFile::read(char* ptr, int64_t n) {
  gcount_ = 0;
  const JSON get_DL_url = fileDownload(dxid_);
  const string url = get_DL_url["url"].get<string>();

  // TODO: make sure all lower-case works.
  if (file_length_ < 0) {
    JSON desc = describe();
    file_length_ = desc["size"].get<int64_t>();
  }

  if (pos_ >= file_length_) {
    gcount_ = 0;
    return;
  }

  int64_t endbyte = file_length_ - 1;
  if (pos_ + n - 1 < endbyte)
    endbyte = pos_ + n - 1;
  else
    eof_ = true;

  HttpHeaders headers;
  headers["Range"] = "bytes=" + boost::lexical_cast<string>(pos_) + "-" + boost::lexical_cast<string>(endbyte);
  pos_ = endbyte + 1;
  
  
  HttpRequest resp;
  makeHTTPRequestForFileReadAndWrite(resp, url, headers, HTTP_GET);

  memcpy(ptr, resp.respData.data(), resp.respData.length());
  gcount_ = resp.respData.length();
}

/////////////////////////////////////////////////////////////////////////////////
void DXFile::startLinearQuery(const int64_t start_byte,
                      const int64_t num_bytes,
                      const int64_t chunk_size,
                      const unsigned max_chunks,
                      const unsigned thread_count) const {
  if (is_closed() == false)
    throw DXFileError("ERROR: Cannot call DXFile::startLinearQuery() on a file in non-closed state");
  stopLinearQuery(); // Stop any previously running linear query
  lq_query_start_ = (start_byte == -1) ? 0 : start_byte;
  lq_query_end_ = (num_bytes == -1) ? describe()["size"].get<int64_t>() : lq_query_start_ + num_bytes;
  lq_chunk_limit_ = chunk_size;
  lq_max_chunks_ = max_chunks;
  lq_next_result_ = lq_query_start_;
  lq_results_.clear();

  const JSON get_DL_url = fileDownload(dxid_);
  lq_url = get_DL_url["url"].get<string>();

  for (unsigned i = 0; i < thread_count; ++i)
    lq_readThreads_.push_back(boost::thread(boost::bind(&DXFile::readChunk_, this)));
}

// Do *NOT* call this function with value of "end" past the (last - 1) byte of file, i.e.,
// the Range: [start,end] should be a valid byte range in file (shouldn't be past the end of file)
void DXFile::getChunkHttp_(int64_t start, int64_t end, string &result) const {
  int64_t last_byte_in_result = start - 1;
 
  while (last_byte_in_result < end) {
    HttpHeaders headers;
    string range = boost::lexical_cast<string>(last_byte_in_result + 1) + "-" + boost::lexical_cast<string>(end);
    headers["Range"] = "bytes=" + range;
    
    HttpRequest resp;
    makeHTTPRequestForFileReadAndWrite(resp, lq_url, headers, HTTP_GET);

    if (result == "")
      result = resp.respData;
    else
      result.append(resp.respData);

    last_byte_in_result += resp.respData.size();   
  }
  assert(result.size() == (end - start + 1));
}

void DXFile::readChunk_() const {
  int64_t start;
  while (true) {
    boost::mutex::scoped_lock qs_lock(lq_query_start_mutex_);
    if (lq_query_start_ >= lq_query_end_)
      break; // We are done fetching all chunks

    start = lq_query_start_;
    lq_query_start_ += lq_chunk_limit_;
    qs_lock.unlock();
    
    int64_t end = std::min((start + lq_chunk_limit_ - 1), lq_query_end_ - 1);
    
    std::string tmp;
    getChunkHttp_(start, end, tmp);
     
    boost::mutex::scoped_lock r_lock(lq_results_mutex_);
    while (lq_next_result_ != start && lq_results_.size() >= lq_max_chunks_) {
      r_lock.unlock();
      boost::this_thread::sleep(boost::posix_time::milliseconds(1));
      r_lock.lock();
    }
    lq_results_[start] = tmp;
    r_lock.unlock();
    boost::this_thread::interruption_point();
  }
}

bool DXFile::getNextChunk(string &chunk) const {
  if (lq_readThreads_.size() == 0) // Linear query was not called
    return false;

  boost::mutex::scoped_lock r_lock(lq_results_mutex_);
  if (lq_next_result_ >= lq_query_end_)
    return false;
  
  while (lq_results_.size() == 0 || (lq_results_.begin()->first != lq_next_result_)) {
    r_lock.unlock();
    usleep(100);
    r_lock.lock();
  }
  chunk = lq_results_.begin()->second;
  lq_results_.erase(lq_results_.begin());
  lq_next_result_ += chunk.size();
  r_lock.unlock();
  return true;
}

void DXFile::stopLinearQuery() const {
  if (lq_readThreads_.size() == 0)
    return;
  for (unsigned i = 0; i < lq_readThreads_.size(); ++i) {
    lq_readThreads_[i].interrupt();
    lq_readThreads_[i].join();
  }
  lq_readThreads_.clear();
  lq_results_.clear();
}
/////////////////////////////////////////////////////////////////////////////////

int64_t DXFile::gcount() const {
  return gcount_;
}

bool DXFile::eof() const {
  return eof_;
}

void DXFile::seek(const int64_t pos) {
  // Check if a file is closed before "seeking"
  if (is_closed() == false) {
    throw DXFileError("ERROR: Cannot call DXFile::seek() when a file is not in 'closed' state");
  }
  pos_ = pos;
  if (pos_ < file_length_)
    eof_ = false;
}

///////////////////////////////////////////////////////////////////////

void DXFile::joinAllWriteThreads_() {
  /* This function ensures that all pending requests are executed and all
   * worker thread are closed after that
   * Brief notes about functioning:
   * --> uploadPartRequestsQueue.size() == 0, ensures that request queue is empty, i.e.,
   *     some worker has picked the last request (note we use term "pick", because 
   *     the request might still be executing the request).
   * --> Once we know that request queue is empty, we issue interrupt() to all threads
   *     Note: interrupt() will only terminate threads, which are waiting on new request.
   *           So only threads which are blocked by .consume() operation will be terminated
   *           immediatly.
   * --> Now we use a condition based on two interleaved counters to wait until all the 
   *     threads have finished the execution. (see writeChunk_() for understanding their usage)
   * --> Once we are sure that all threads have finished the requests, we join() them.
   *     Since interrupt() was already issued, thus join() terminates them instantly.
   *     Note: Most of them would have been already terminated (since after issuing 
   *           interrupt(), they will be terminated when they start waiting on consume()). 
   *           It's ok to join() terminated threads.
   * --> We clear the thread pool (vector), and reset the counters.
   */

  if (writeThreads.size() == 0)
    return; // Nothing to do (no thread has been started)
  
  // To avoid race condition
  // particularly the case when produce() has been called, but thread is still waiting on consume()
  // we don't want to incorrectly issue interrupt() that time
  while (uploadPartRequestsQueue.size() != 0) {
    usleep(100);
  }

  for (unsigned i = 0; i < writeThreads.size(); ++i)
    writeThreads[i].interrupt();
 
  boost::mutex::scoped_lock cl(countThreadsMutex); 
  cl.unlock();
  while (true) {
    cl.lock();
    if ((countThreadsNotWaitingOnConsume == 0) &&
        (countThreadsWaitingOnConsume == (int) writeThreads.size())) {
      cl.unlock();
      break;
    }
    cl.unlock();
    usleep(100);
  }
  
  for (unsigned i = 0; i < writeThreads.size(); ++i)
    writeThreads[i].join();
  
  writeThreads.clear();
  // Reset the counts
  countThreadsWaitingOnConsume = 0;
  countThreadsNotWaitingOnConsume = 0;
}

// This function is what each of the worker thread executes
void DXFile::writeChunk_() {
  try {
    boost::mutex::scoped_lock cl(countThreadsMutex); 
    cl.unlock();
    /* This function is executed throughtout the lifetime of an addRows worker thread
     * Brief note about various constructs used in the function:
     * --> uploadPartRequestsQueue.consume() will block if no pending requests to be
     *     excuted are available.
     * --> uploadPart() does the actual upload of rows.
     * --> We use two interleaved counters (countThread{NOT}WaitingOnConsume) to
     *     know when it is safe to terminate the threads (see joinAllWriteThreads_()).
     *     We want to terminate only when thread is waiting on .consume(), and not
     *     when gtableAddRows() is being executed.
     */
     // See C++11 working draft for details about atomics (used for counterS)
     // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2012/n3337.pdf
    while (true) {
      cl.lock();
      countThreadsWaitingOnConsume++;
      cl.unlock();
      pair<string, int> elem = uploadPartRequestsQueue.consume();
      cl.lock();
      countThreadsNotWaitingOnConsume++;
      countThreadsWaitingOnConsume--;
      cl.unlock();
      uploadPart(elem.first.data(), elem.first.size(), elem.second);
      cl.lock();
      countThreadsNotWaitingOnConsume--;
      cl.unlock();
    }
  }
  catch (const boost::thread_interrupted &ti) 
  {
    return;
  }

}

/* This function creates new worker thread for addRows
 * Usually it wil be called once for a series of addRows() and then close() request
 * However, if we call flush() in between, then we destroy any existing threads, thus 
 * threads will be recreated for any further addRows() request
 */
void DXFile::createWriteThreads_() {
  if (writeThreads.size() == 0) {
    for (int i = 0; i < MAX_WRITE_THREADS; ++i) {
      writeThreads.push_back(boost::thread(boost::bind(&DXFile::writeChunk_, this)));
    }
  }
}

// NOTE: If needed, optimize in the future to not have to copy to
// append to buffer_ before uploading the next part.
void DXFile::write(const char* ptr, int64_t n) {
  int64_t remaining_buf_size = max_buf_size_ - buffer_.tellp();
//  std::cerr<<"\nwrite(): In write .. adding "<<n<<" bytes";
//  std::cerr<<"\nBuffer size = "<<buffer_.tellp();
  if (n < remaining_buf_size) {
    buffer_.write(ptr, n);
  } else {
    buffer_.write(ptr, remaining_buf_size);
//    std::cerr<<"Hitting buffer size in write(), length = buffer_.tellp()"; 
    // Create thread pool (if not already created)
    if (writeThreads.size() == 0)
      createWriteThreads_();

    // add upload request for this part to blocking queue
    uploadPartRequestsQueue.produce(make_pair(buffer_.str(), cur_part_)); 
    buffer_.str(string()); // clear the buffer
    cur_part_++; // increment the part number for next request
    
    // Add remaining data to buffer (will be added in next call)
    write(ptr + remaining_buf_size, n - remaining_buf_size);
  }
}

void DXFile::write(const string &data) {
  write(data.data(), data.size());
}

void DXFile::flush() {
  if (buffer_.tellp() > 0) {
    // We have some data to flush before joining all the threads
    // Create thread pool (if not already created)
    if (writeThreads.size() == 0)
       createWriteThreads_();
    uploadPartRequestsQueue.produce(make_pair(buffer_.str(), cur_part_));
    cur_part_++;
  }

  // Now join all write threads
  joinAllWriteThreads_();
  buffer_.str(string());
}

//////////////////////////////////////////////////////////////////////

void DXFile::uploadPart(const string &data, const int index) {
  uploadPart(data.data(), data.size(), index);
}

void DXFile::uploadPart(const char *ptr, int64_t n, const int index) {
  JSON input_params(JSON_OBJECT);
  if (index >= 1)
    input_params["index"] = index;

  const JSON resp = fileUpload(dxid_, input_params);
  HttpHeaders req_headers;
  req_headers["Content-Length"] = boost::lexical_cast<string>(n);
  
  HttpRequest resp2;
  makeHTTPRequestForFileReadAndWrite(resp2, resp["url"].get<string>(), HttpHeaders(), HTTP_POST, ptr, n);
}

bool DXFile::is_open() const {
  // If is_closed_ is true, then file cannot be "open"
  // Since initial value of is_closed_ = false, and file cannot be "open" after
  // being "closed" once.
  if (is_closed_ == true)
    return false;
  const JSON resp = describe();
  return (resp["state"].get<string>() == "open");
}

bool DXFile::is_closed() const {
  // If is_closed_ is set to true, then we do not need to check
  // since a file cannot be un-"closed" after closing.
  if (is_closed_ == true)
    return true;

  const JSON resp = describe();
  return (is_closed_ = (resp["state"].get<string>() == "closed"));
}

void DXFile::close(const bool block) {
  flush();
  fileClose(dxid_);
  if (block)
    waitOnState("closed");
}

void DXFile::waitOnClose() const {
  waitOnState("closed");
}

DXFile DXFile::openDXFile(const string &dxid) {
  return DXFile(dxid);
}

DXFile DXFile::newDXFile(const string &media_type,
                         const JSON &data_obj_fields) {
  DXFile dxfile;
  dxfile.create(media_type, data_obj_fields);
  return dxfile;
}

void DXFile::downloadDXFile(const string &dxid, const string &filename,
                            int64_t chunksize) {
  DXFile dxfile(dxid);
  if (!dxfile.is_closed())
    throw DXFileError("Error: Remote file must be in 'closed' state before it can be downloaded");

  ofstream localfile(filename.c_str());
  dxfile.startLinearQuery(-1, -1, chunksize);
  std::string chunk;
  while (dxfile.getNextChunk(chunk))
    localfile.write(chunk.data(), chunk.size());

  localfile.close();
}

static string getBaseName(const string& filename) {
  size_t lastslash = filename.find_last_of("/\\");
  return filename.substr(lastslash+1);
}

DXFile DXFile::uploadLocalFile(const string &filename, const string &media_type,
                               const JSON &data_obj_fields, bool waitForClose) {
  DXFile dxfile = newDXFile(media_type, data_obj_fields);
  ifstream localfile(filename.c_str());
  char * buf = new char [DXFile::max_buf_size_];
  try {
    while (!localfile.eof()) {
      localfile.read(buf, DXFile::max_buf_size_);
      int64_t num_bytes = localfile.gcount();
      dxfile.write(buf, num_bytes);
    }
  } catch (...) {
    delete [] buf;
    localfile.close();
    throw;
  }
  delete[] buf;
  localfile.close();
  JSON name_prop(JSON_OBJECT);
  name_prop["name"] = getBaseName(filename);
  dxfile.setProperties(name_prop);
  dxfile.close(waitForClose);
  return dxfile;
}

DXFile DXFile::clone(const string &dest_proj_id,
                     const string &dest_folder) const {
  clone_(dest_proj_id, dest_folder);
  return DXFile(dxid_, dest_proj_id);
}
