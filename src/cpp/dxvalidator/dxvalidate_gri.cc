#include "dxvalidate_gri.h"

using namespace dx;

void GriColumnsHandler::Init() {
  clearColumns();

  addColumn("chr", "string", 0);
  addColumn("lo" , "integer", 0);
  addColumn("hi" , "integer", 0);
}

bool GriRowValidator::initFlatFile(const JSON &details) {
  try {
    flatFile.setIDs(details["flat_sequence_file"]["$dnanexus_link"].get<string>());

    JSON desc;
    try {
      desc = flatFile.describe();
    } catch (DXAPIError &e) {
      if (e.resp_code == 404) return msg->setError("CONTIGSET_INVALID");
      return msg->setDXError(e.msg, "FLAT_SEQUENCE_FETCH_FAIL");
    }

    if (desc["class"].get<string>() != "file") return msg->setError("CONTIGSET_INVALID");
    if (desc["state"].get<string>() != "closed") return msg->setError("CONTIGSET_INVALID");

    return true;
  } catch (JSONException &e) {
    return msg->setError("CONTIGSET_INVALID");
  }
}

GriRowValidator::GriRowValidator(const string &contigset_id, ValidateInfo *m) : GTableRowValidator(m) {
  ready = fetchContigSets(contigset_id);

  chrCols.clear(); loCols.clear(); hiCols.clear(); chr_valid.clear();
  chrCols.push_back("chr"); loCols.push_back("lo"); hiCols.push_back("hi"); chr_valid.push_back(true);
}

void GriRowValidator::AddGri(const string &chr, const string &lo, const string &hi) {
  chrCols.push_back(chr); loCols.push_back(lo); hiCols.push_back(hi); chr_valid.push_back(true);
}

bool GriRowValidator::fetchContigSets(const string &source_id) {
  DXRecord object(source_id);
  JSON details;
  try {
    details = object.getDetails();
  } catch (DXAPIError &e) {
    if (e.resp_code == 404) return msg->setError("CONTIGSET_INVALID");
    return msg->setDXError(e.msg, "CONTIGSET_FETCH_FAIL");
  }

  try {
    hasOffset = false;
    hasFlat = details.has("flat_sequence_file");
    if (hasFlat) {
      if (! initFlatFile(details)) return false;

      hasOffset = details["contigs"].has("offsets");
      if (! hasOffset) return msg->setError("CONTIGSET_INVALID");
    }

    int n = details["contigs"]["names"].size();

    indices.clear();
    sizes.resize(n); offsets.resize(n);

    for (int i = 0; i < n; i++) {
      indices[details["contigs"]["names"][i].get<string>()] = i;
      sizes[i] = int64_t(details["contigs"]["sizes"][i]);
      if (hasOffset) offsets[i] = int64_t(details["contigs"]["offsets"][i]);
    }

  } catch (JSONException &e) {
    return msg->setError("CONTIGSET_INVALID");
  }

  return true;
}

bool GriRowValidator::FetchSeq(int64_t pos, char *buffer, int bufSize) {
  try {
    flatFile.seek(pos);
    flatFile.read(buffer, bufSize);
  } catch (DXError &e) {
    return msg->setDXError(e.msg, "FLAT_SEQUENCE_FETCH_FAIL");
  }

  return true;
}

bool GriRowValidator::validateGri(const string &chr, int64_t lo, int64_t hi, int k) {
  if (lo < 0) {
    msg->setData(loCols[k], 1);
    return msg->setRowError("LO_TOO_SMALL");
  }

  if (lo > hi) {
    msg->setData(loCols[k], 1);
    msg->setData(hiCols[k], 2);
    return msg->setRowError("LO_TOO_LARGE");
  }
  
  map<string, int>::iterator it = indices.find(chr);
  if (it != indices.end()) {
    chrIndex = it->second;
  
    if (hi > sizes[chrIndex]) {
      msg->setData(hiCols[k], 1);
      return msg->setRowError("HI_TOO_LARGE");
    }
  } else {
    if (chr_valid[k]) {
      msg->setData(chrCols[k], 1);
      msg->addRowWarning("CHR_INVALID");
      chr_valid[k] = false;
    }
  }

  return true;
}

bool GriValidator::validateTypes() {
  GTableValidator::validateTypes();
  if (! types.Has("gri")) return msg->setError("TYPE_NOT_GRI");
  if (! hasGenomicIndex()) return msg->setError("GRI_INDEX_MISSING");
  return true;
}

bool GriValidator::validateColumns() {
  columns = new GriColumnsHandler();
  bool ret_val = processColumns();
  delete columns;
  return ret_val;
}

bool GriValidator::validateDetails() {
  if (! details.has("original_contigset")) return msg->setError("CONTIGSET_MISSING");
  if (details["original_contigset"].type() != JSON_OBJECT) return msg->setError("CONTIGSET_INVALID");
  if (! details["original_contigset"].has("$dnanexus_link")) return msg->setError("CONTIGSET_INVALID");
  if (details["original_contigset"]["$dnanexus_link"].type() != JSON_STRING) return msg->setError("CONTIGSET_INVALID");
  return true;
}

bool GriValidator::hasGenomicIndex() {
  if (! desc.has("indices")) return false; 
  for (int i = 0; i < desc["indices"].size(); i++) {
    if (desc["indices"][i]["name"] != "gri") continue;
    if (desc["indices"][i]["type"] != "genomic") return false;
    if (! desc["indices"][i].has("chr")) return false;
    if (desc["indices"][i]["chr"].get<string>() != "chr") return false;
    if (! desc["indices"][i].has("lo")) return false;
    if (desc["indices"][i]["lo"].get<string>() != "lo") return false;
    if (! desc["indices"][i].has("hi")) return false;
    if (desc["indices"][i]["hi"].get<string>() != "hi") return false;
    return true;
  }

  return false;
}
