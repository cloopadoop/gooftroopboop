/**
 * VGMTrans (c) - 2002-2021
 * Licensed under the zlib license
 * See the included LICENSE for more information
 */

#pragma once

#include "Root.h"
#include <set>
#include <filesystem>

#define CLI_APP_NAME "GTBoop"

namespace fs = std::filesystem;

class CLIGTBRoot : public GTBRoot {

public:

  size_t numCollections() {
    return GTBColls().size();
  }

  void displayUsage();

  void displayHelp();

  bool makeOutputDir();

  bool exportAllCollections();

  bool exportCollection(GTBColl* coll);

  bool saveMidi(const GTBColl *coll);

  bool saveSF2(GTBColl *coll);

  bool saveDLS(GTBColl *coll);

  bool openRawFile(const std::string &filename) override;

  bool init() override;

  void UI_setRootPtr(GTBRoot** theRoot) override;

  void UI_log(LogItem* theLog) override;

  std::string UI_getSaveFilePath(const std::string& suggestedFilename,
                                 const std::string& extension = "") override;

  std::string UI_getSaveDirPath(const std::string& suggestedDir = "") override;

  std::set<fs::path> inputFiles = {};
  fs::path outputDir = fs::path(".");
};

extern CLIGTBRoot cliroot;