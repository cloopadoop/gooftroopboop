/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include <fstream>

#include "Root.h"
#include "RawFile.h"
#include "GTBColl.h"
#include "GTBFile.h"
#include "GTBSeq.h"
#include "GTBInstrSet.h"
#include "GTBSampColl.h"
#include "GTBMiscFile.h"
#include "Format.h"
#include "Scanner.h"
#include "Matcher.h"

#include "FileLoader.h"
#include "LoaderManager.h"
#include "ScannerManager.h"
#include "LogManager.h"

#include <filesystem>
#include <cctype>
#include <algorithm>

GTBRoot *pRoot;

GTBFile* variantToGTBFile(GTBFileVariant variant) {
  GTBFile *GTBFilePtr = nullptr;
  std::visit([&GTBFilePtr](auto *GTB) { GTBFilePtr = static_cast<GTBFile *>(GTB); }, variant);
  return GTBFilePtr;
}

GTBFileVariant GTBFileToVariant(GTBFile* file) {
  if (auto seq = dynamic_cast<GTBSeq*>(file)) {
    return seq;
  } else if (auto instrSet = dynamic_cast<GTBInstrSet*>(file)) {
    return instrSet;
  } else if (auto sampColl = dynamic_cast<GTBSampColl*>(file)) {
    return sampColl;
  } else if (auto miscFile = dynamic_cast<GTBMiscFile*>(file)) {
    return miscFile;
  } else {
    throw std::runtime_error("Unknown GTBFile subclass");
  }
}

bool GTBRoot::init() {
    UI_setRootPtr(&pRoot);
    return true;
}

/* Opens up a file from the filesystem and scans it.
 * Returns bool indicating if GTBFiles were found. */
bool GTBRoot::openRawFile(const std::string &filePath) {
  RawFile* newFile = nullptr;
  try {
    auto ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".sfc" || ext == ".spc") {
      newFile = new EditableRawFile(filePath);
    } else {
      newFile = new DiskFile(filePath);
    }
  } catch (...) {
    UI_toast("Error opening file at path: " + filePath, ToastType::Error);
    return false;
  }
  size_t GTBFileCountBefore = GTBFiles().size();
  if (!setupNewRawFile(newFile)) {
    delete newFile;
  }
  return GTBFiles().size() > GTBFileCountBefore;
}

/* Creates a new file backed by RAM */
bool GTBRoot::createVirtFile(const uint8_t *databuf, uint32_t fileSize, const std::string& filename,
                             const std::string &parRawFileFullPath, const GTBTag& tag) {
  assert(fileSize != 0);

  auto newVirtFile = new VirtFile(databuf, fileSize, filename,
    parRawFileFullPath, tag);

  if (!setupNewRawFile(newVirtFile)) {
    delete newVirtFile;
    return false;
  }
  return true;
}

// Applies loaders and scanners to a rawfile, loading any discovered files
// returns true if files were discovered
bool GTBRoot::setupNewRawFile(RawFile *newRawFile) {
  UI_onBeginLoadRawFile();
  if (newRawFile->useLoaders()) {
    for (const auto &l : LoaderManager::get().loaders()) {
      l->apply(newRawFile);
      auto res = l->results();

      /* If the loader extracted anything, we shouldn't have to scan */
      if (!res.empty()) {
        newRawFile->setUseScanners(false);

        for (const auto &file : res) {
          if (!setupNewRawFile(file)) {
            delete file;
          }
        }
      }
    }
  }

  if (newRawFile->useScanners()) {
    /*
     * Make use of the extension to run only a subset of scanners.
     * Unsure how good of an idea this is
     */
    auto specific_scanners =
      ScannerManager::get().scannersWithExtension(newRawFile->extension());
    if (!specific_scanners.empty()) {
      for (const auto &scanner : specific_scanners) {
        scanner->scan(newRawFile);
        if (auto matcher = scanner->format()->matcher) {
          matcher->onFinishedScan(newRawFile);
        }
      }
    } else {
      for (const auto &scanner : ScannerManager::get().scanners()) {
        scanner->scan(newRawFile);
        if (auto matcher = scanner->format()->matcher) {
          matcher->onFinishedScan(newRawFile);
        }
      }
    }
  }

  bool foundFiles = !newRawFile->containedGTBFiles().empty();
  if (foundFiles) {
    m_rawfiles.emplace_back(newRawFile);
    UI_addRawFile(newRawFile);
  }

  UI_onEndLoadRawFile();
  return foundFiles;
}

bool GTBRoot::closeRawFile(RawFile *targFile) {
  if (!targFile) {
    return false;
  }

  auto file = std::ranges::find(m_rawfiles, targFile);
  if (file != m_rawfiles.end()) {
    auto &GTBfiles = (*file)->containedGTBFiles();
    UI_beginRemoveGTBFiles();
    for (const auto & GTBfile : GTBfiles) {
      removeGTBFile(*GTBfile, false);
    }
    UI_endRemoveGTBFiles();

    m_rawfiles.erase(file);

    UI_closeRawFile(targFile);
  } else {
    L_WARN("Requested deletion for RawFile but it was not found");
    return false;
  }
  delete targFile;
  return true;
}

void GTBRoot::addGTBFile(
  std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file) {
  m_GTBfiles.push_back(file);
  L_INFO("Loaded {} successfully.", variantToGTBFile(file)->name());
  UI_addGTBFile(file);
}

// Removes a GTBFile from the interface.  The UI_RemoveGTBFile will handle the
// interface-specific stuff
void GTBRoot::removeGTBFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file, bool bRemoveEmptyRawFile) {
  auto targFile = variantToGTBFile(file);
  // First we should call the format's onClose handler in case it needs to use
  // the RawFile before we close it (FilenameMatcher, for ex)
  if (Format *fmt = targFile->format()) {
    fmt->onCloseFile(file);
  }

  auto iter = std::ranges::find(m_GTBfiles, file);

  if (iter != m_GTBfiles.end()) {
    UI_removeGTBFile(targFile);
    m_GTBfiles.erase(iter);
  } else {
    L_WARN("Requested deletion for GTBFile but it was not found");
  }

  while (!targFile->assocColls.empty()) {
    removeGTBColl(targFile->assocColls.back());
  }

  if (bRemoveEmptyRawFile) {
    const auto rawFile = targFile->rawFile();
    rawFile->removeContainedGTBFile(file);
    if (rawFile->containedGTBFiles().empty()) {
      closeRawFile(rawFile);
    }
  }
  delete targFile;
}

void GTBRoot::addGTBColl(GTBColl *theColl) {
    m_GTBcolls.push_back(theColl);
    UI_addGTBColl(theColl);
}

void GTBRoot::removeGTBColl(GTBColl *targColl) {
  auto iter = std::ranges::find(m_GTBcolls, targColl);
  if (iter != m_GTBcolls.end())
    m_GTBcolls.erase(iter);
  else
    L_WARN("Requested deletion for GTBColl but it was not found");

  targColl->removeFileAssocs();
  UI_removeGTBColl(targColl);
  delete targColl;
}

// This virtual function is called whenever a GTBFile is added to the interface.
// By default, it simply sorts out what type of file was added and then calls a more
// specific virtual function for the file type.  It is virtual in case a user-interface
// wants do something universally whenever any type of GTBFiles is added.
void GTBRoot::UI_addGTBFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> file) {
    if(auto seq = std::get_if<GTBSeq *>(&file)) {
        UI_addGTBSeq(*seq);
    } else if(auto instr = std::get_if<GTBInstrSet *>(&file)) {
        UI_addGTBInstrSet(*instr);
    } else if(auto sampcoll = std::get_if<GTBSampColl *>(&file)) {
        UI_addGTBSampColl(*sampcoll);
    } else if(auto misc = std::get_if<GTBMiscFile *>(&file)) {
        UI_addGTBMisc(*misc);
    }
}

// Given a pointer to a buffer of data, size, and a filename, this function writes the data
// into a file on the filesystem.
bool GTBRoot::UI_writeBufferToFile(const std::string &filepath, uint8_t *buf, size_t size) {
    std::ofstream outfile(filepath, std::ios::out | std::ios::trunc | std::ios::binary);

    if (!outfile.is_open()) {
      L_ERROR(std::string("Error: could not open file " + filepath + " for writing").c_str());
        return false;
    }

    outfile.write(reinterpret_cast<char *>(buf), size);
    outfile.close();
    return true;
}

// Adds a log item to the interface. The UI_AddLog function will handle the interface-specific stuff
void GTBRoot::log(LogItem *theLog) {
  UI_log(theLog);
}

std::string GTBRoot::UI_getResourceDirPath() {
  return std::string(std::filesystem::current_path().generic_string());
}
