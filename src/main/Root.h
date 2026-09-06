/*
 * VGMTrans (c) 2002-2019
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */
#pragma once

#include <variant>
#include <memory>

#include "common.h"
#include "GTBTag.h"

class GTBScanner;
class GTBColl;
class GTBItem;
class GTBFile;
class RawFile;
class GTBSeq;
class GTBInstrSet;
class GTBSampColl;
class GTBMiscFile;
class LogItem;

constexpr int DEFAULT_TOAST_DURATION = 8000;

template <typename T>
T* variantToType(GTBFileVariant variant) {
  T* GTBFilePtr = nullptr;
  std::visit([&GTBFilePtr](auto *GTB) { GTBFilePtr = dynamic_cast<T*>(GTB); }, variant);
  return GTBFilePtr;
}
GTBFile* variantToGTBFile(GTBFileVariant variant);
GTBFileVariant GTBFileToVariant(GTBFile* GTBfile);

enum class ToastType { Info, Warning, Error, Success };

class GTBRoot {
public:
  GTBRoot() = default;
  virtual ~GTBRoot() = default;

  virtual bool init();
  virtual bool openRawFile(const std::string &filePath);
  bool createVirtFile(const uint8_t* databuf, uint32_t fileSize, const std::string& filename,
                      const std::string& parRawFileFullPath = "", const GTBTag& tag = GTBTag());
  bool setupNewRawFile(RawFile* newRawFile);
  bool closeRawFile(RawFile *targFile);
  void addGTBFile(GTBFileVariant file);
  void removeGTBFile(GTBFileVariant file, bool bRemoveEmptyRawFile = true);
  void addGTBColl(GTBColl *theColl);
  void removeGTBColl(GTBColl *theFile);
  void log(LogItem *theLog);

  virtual std::string UI_getResourceDirPath();
  virtual void UI_setRootPtr(GTBRoot **theRoot) = 0;
  virtual void UI_addRawFile(RawFile *) {}
  virtual void UI_closeRawFile(RawFile *) {}

  virtual void UI_onBeginLoadRawFile() {}
  virtual void UI_onEndLoadRawFile() {}
  virtual void UI_addGTBFile(GTBFileVariant file);
  virtual void UI_addGTBSeq(GTBSeq *) {}
  virtual void UI_addGTBInstrSet(GTBInstrSet *) {}
  virtual void UI_addGTBSampColl(GTBSampColl *) {}
  virtual void UI_addGTBMisc(GTBMiscFile *) {}
  virtual void UI_addGTBColl(GTBColl *) {}
  virtual void UI_removeGTBFile(GTBFile *) {}
  virtual void UI_beginRemoveGTBFiles() {}
  virtual void UI_endRemoveGTBFiles() {}
  virtual void UI_log(LogItem *) { }
  virtual void UI_toast(const std::string& message, ToastType type = ToastType::Info,
                        int duration_ms = DEFAULT_TOAST_DURATION) {}

  virtual void UI_removeGTBColl(GTBColl *) {}
  virtual void UI_beginRemoveGTBColls() {}
  virtual void UI_endRemoveGTBColls() {}
  virtual void UI_addItem(GTBItem *, GTBItem *, const std::string &, void *) {}
  virtual std::string UI_getSaveFilePath(const std::string &suggestedFilename,
                                         const std::string &extension = "") = 0;
  virtual std::string UI_getSaveDirPath(const std::string &suggestedDir = "") = 0;
  virtual bool UI_writeBufferToFile(const std::string &filepath, uint8_t *buf, size_t size);

  const std::vector<RawFile*>& rawFiles() { return m_rawfiles; }
  const std::vector<GTBFileVariant>& GTBFiles() { return m_GTBfiles; }
  const std::vector<GTBColl*>& GTBColls() { return m_GTBcolls; }

private:
  std::vector<RawFile *> m_rawfiles;
  std::vector<GTBFileVariant> m_GTBfiles;
  std::vector<GTBColl *> m_GTBcolls;
};

extern GTBRoot *pRoot;
