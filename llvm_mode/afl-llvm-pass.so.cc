/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   Copyright 2015, 2016 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.

 */

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <list>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/CFGPrinter.h"

#if defined(LLVM34)
#include "llvm/DebugInfo.h"
#else
#include "llvm/IR/DebugInfo.h"
#endif

#if defined(LLVM34) || defined(LLVM35) || defined(LLVM36)
#define LLVM_OLD_DEBUG_API
#endif

using namespace llvm;

cl::opt<std::string> DistanceFile(
    "distance",
    cl::desc("Distance file containing the distance of each basic block to the provided targets."),
    cl::value_desc("filename")
);

cl::opt<std::string> TargetsFile(
    "targets",
    cl::desc("Input file containing the target lines of code."),
    cl::value_desc("targets"));

cl::opt<std::string> OutDirectory(
    "outdir",
    cl::desc("Output directory where Ftargets.txt, Fnames.txt, and BBnames.txt are generated."),
    cl::value_desc("outdir"));
	
cl::opt<std::string> BasicBlockInfoFile(
    "blockinfo",
    cl::desc("BasicBlockInfoFile files containing the number of all basic blocks."),
    cl::value_desc("blockinfo"));

namespace {

    class AFLCoverage : public ModulePass {

    public:

        static char ID;
        AFLCoverage() : ModulePass(ID) { }

        bool runOnModule(Module &M) override;

    };

}

static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}

char AFLCoverage::ID = 0;

bool AFLCoverage::runOnModule(Module &M) {

	bool is_aflgo = false;
	bool is_aflgo_preprocessing = false;

	if (!TargetsFile.empty() && !DistanceFile.empty() && !BasicBlockInfoFile.empty()) {
		FATAL("Cannot specify  '-targets' and '-distance' and '-blockinfo'!");
		return false;
	}

	std::list<std::string> targets;
	std::map<std::string, int> bb_to_dis;
	std::vector<std::string> basic_blocks;
	std::map<std::string, int> bb_to_number;

	if (!TargetsFile.empty()) {

		if (OutDirectory.empty()) {
			FATAL("Provide output directory '-outdir <directory>'");
			return false;
		}

		std::ifstream targetsfile(TargetsFile);
		std::string line;
		while (std::getline(targetsfile, line))
			targets.push_back(line);
			targetsfile.close();

		is_aflgo_preprocessing = true;

	} else if (!DistanceFile.empty()&& !BasicBlockInfoFile.empty()) {

		std::ifstream cf (DistanceFile.c_str());
		if (cf.is_open()) {

			std::string line;
			while (getline(cf,line)) {

				std::size_t pos = line.find(",");
				std::string bb_name = line.substr(0, pos);
				int bb_dis = (int) (100.0 * atof(line.substr(pos + 1, line.length()).c_str()));

				bb_to_dis.insert(std::pair<std::string,int>(bb_name, bb_dis) );
				basic_blocks.push_back(bb_name);

			}
			cf.close();
		}	
		std::ifstream cff (BasicBlockInfoFile.c_str());
		if (cff.is_open()) {
			
			std::string line;
			while (getline(cff,line)) {

				std::size_t position = line.find(",");
				std::string bbname = line.substr(0, position);
				int bbnumber = (int) (100.0 * atof(line.substr(position + 1, line.length()).c_str()));

				bb_to_number.insert(std::pair<std::string,int>(bbname, bbnumber) );
        	
			}
			cff.close();

		    is_aflgo = true;

	    }
		else {
          FATAL("Unable to find %s.", DistanceFile.c_str());
          return false;
        }

    }

	LLVMContext &C = M.getContext();

	IntegerType *Int8Ty  = IntegerType::getInt8Ty(C);
	IntegerType *Int32Ty = IntegerType::getInt32Ty(C);
	IntegerType *Int64Ty = IntegerType::getInt64Ty(C);

	/* Show a banner */

	char be_quiet = 0;

	if (isatty(2) && !getenv("AFL_QUIET")) {

		if (is_aflgo || is_aflgo_preprocessing)
			SAYF(cCYA "aflgo-llvm-pass (yeah!) " cBRI VERSION cRST " (%s mode)\n",
				(is_aflgo_preprocessing ? "preprocessing" : "distance instrumentation"));
		else
			SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");


	} 
	else 
		be_quiet = 1;

	/* Decide instrumentation ratio */

	char* inst_ratio_str = getenv("AFL_INST_RATIO");
	unsigned int inst_ratio = 100;

	if (inst_ratio_str) {

		if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
			inst_ratio > 100)
			FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

	}

  /* Default: Not selecitive */
	char* is_selective_str = getenv("AFLGO_SELECTIVE");
	unsigned int is_selective = 0;

	if (is_selective_str && sscanf(is_selective_str, "%u", &is_selective) != 1)
		FATAL("Bad value of AFLGO_SELECTIVE (must be 0 or 1)");

	char* dinst_ratio_str = getenv("AFLGO_INST_RATIO");
	unsigned int dinst_ratio = 100;

	if (dinst_ratio_str) {

		if (sscanf(dinst_ratio_str, "%u", &dinst_ratio) != 1 || !dinst_ratio ||
			dinst_ratio > 100)
			FATAL("Bad value of AFLGO_INST_RATIO (must be between 1 and 100)");

	}

	/* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

	GlobalVariable *AFLMapPtr =
		new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

	GlobalVariable *AFLPrevLoc = new GlobalVariable(
		M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
		0, GlobalVariable::GeneralDynamicTLSModel, 0, false);
	
	
	GlobalVariable *AFLGoMapPtr =
		  new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
							 GlobalValue::ExternalLinkage, 0, "__aflgo_area_ptr");
		  
	GlobalVariable *AFLGoPrevLoc = new GlobalVariable(
		  M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__aflgo_prev_loc",
		  0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

	  /* Instrument all the things! */

    int inst_blocks = 0;

	if (is_aflgo_preprocessing) {

		std::ofstream bbnames;
		std::ofstream bbcalls;
		std::ofstream fnames;
		std::ofstream ftargets;
		struct stat sb;
		bbnames.open(OutDirectory + "/BBnames.txt", std::ofstream::out | std::ofstream::app);
		bbcalls.open(OutDirectory + "/BBcalls.txt", std::ofstream::out | std::ofstream::app);
		fnames.open(OutDirectory + "/Fnames.txt", std::ofstream::out | std::ofstream::app);
		ftargets.open(OutDirectory + "/Ftargets.txt", std::ofstream::out | std::ofstream::app);

		/* Create dot-files directory */
		std::string dotfiles(OutDirectory + "/dot-files");
		if (stat(dotfiles.c_str(), &sb) != 0) {
		  const int dir_err = mkdir(dotfiles.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		  if (-1 == dir_err)
			FATAL("Could not create directory %s.", dotfiles.c_str());
		}

		for (auto &F : M) {
			bool has_BBs = false;
			std::string funcName = F.getName();

			/* Black list of function names */
			std::vector<std::string> blacklist = {
				"asan.",
				"llvm.",
				"sancov.",
				"free"
				"malloc",
				"calloc",
				"realloc"
			};
			for (std::vector<std::string>::size_type i = 0; i < blacklist.size(); i++)
				if (!funcName.compare(0, blacklist[i].size(), blacklist[i]))
					continue;

			bool is_target = false;
			for (auto &BB : F) {

				TerminatorInst *TI = BB.getTerminator();
				IRBuilder<> Builder(TI);

				std::string bb_name("");
				std::string filename;
				unsigned line;

				for (auto &I : BB) {
					#ifdef LLVM_OLD_DEBUG_API
						DebugLoc Loc = I.getDebugLoc();
						if (!Loc.isUnknown())  {

							DILocation cDILoc(Loc.getAsMDNode(M.getContext()));
							DILocation oDILoc = cDILoc.getOrigLocation();

							line = oDILoc.getLineNumber();
							filename = oDILoc.getFilename().str();

							if (filename.empty()) {
								line = cDILoc.getLineNumber();
								filename = cDILoc.getFilename().str();
							}
					#else

							if (DILocation *Loc = I.getDebugLoc()) {
									line = Loc->getLine();
									filename = Loc->getFilename().str();

									if (filename.empty()) {
										DILocation *oDILoc = Loc->getInlinedAt();
										if (oDILoc) {
											line = oDILoc->getLine();
											filename = oDILoc->getFilename().str();
										}
									}

						#endif /* LLVM_OLD_DEBUG_API */

								/* Don't worry about external libs */
								std::string Xlibs("/usr/");
								if (filename.empty() || line == 0 || !filename.compare(0, Xlibs.size(), Xlibs))
									continue;

								if (bb_name.empty()) {

									std::size_t found = filename.find_last_of("/\\");
									if (found != std::string::npos)
										filename = filename.substr(found + 1);

										bb_name = filename + ":" + std::to_string(line);

								}

								if (!is_target) {
									for (std::list<std::string>::iterator it = targets.begin(); it != targets.end(); ++it) {

										std::string target = *it;
										std::size_t found = target.find_last_of("/\\");
										if (found != std::string::npos)
											target = target.substr(found + 1);

										std::size_t pos = target.find_last_of(":");
										std::string target_file = target.substr(0, pos);
										unsigned int target_line = atoi(target.substr(pos + 1).c_str());

										if (!target_file.compare(filename) && target_line == line)
											is_target = true;

									}
								}


								if (auto *c = dyn_cast<CallInst>(&I)) {

								std::size_t found = filename.find_last_of("/\\");
								if (found != std::string::npos)
								filename = filename.substr(found + 1);

								if (c->getCalledFunction()) {
									std::string called = c->getCalledFunction()->getName().str();

									bool blacklisted = false;
									for (std::vector<std::string>::size_type i = 0; i < blacklist.size(); i++) {
										if (!called.compare(0, blacklist[i].size(), blacklist[i])) {
											blacklisted = true;
											break;
										}
									}
									if (!blacklisted)
										bbcalls << bb_name << "," << called << "\n";
								}
							}
			
							}
						}

						if (!bb_name.empty()) {

							BB.setName(bb_name + ":");
							if (!BB.hasName()) {
								std::string newname = bb_name + ":";
								Twine t(newname);
								SmallString<256> NameData;
								StringRef NameRef = t.toStringRef(NameData);
								BB.setValueName(ValueName::Create(NameRef));
							}
	//	  std::string bbname_and_id = BB.getName().str()+ "," + std::to_string(bbidnum);
		  
							bbnames << BB.getName().str() << "\n";
							has_BBs = true;
	//		  bbidnum++;
							#ifdef AFLGO_TRACING
								Value *bbnameVal = Builder.CreateGlobalStringPtr(bb_name);
								Type *Args[] = {
									Type::getInt8PtrTy(M.getContext()) //uint8_t* bb_name
								};
								FunctionType *FTy = FunctionType::get(Type::getVoidTy(M.getContext()), Args, false);
								Constant *instrumented = M.getOrInsertFunction("llvm_profiling_call", FTy);
								Builder.CreateCall(instrumented, {bbnameVal});
							#endif

						}
				}

				if (has_BBs) {
					/* Print CFG */
					std::string cfgFileName = dotfiles + "/cfg." + funcName + ".dot";
					struct stat buffer;
					if (stat (cfgFileName.c_str(), &buffer) != 0) {
						FILE *cfgFILE = fopen(cfgFileName.c_str(), "w");
						if (cfgFILE) {
							raw_ostream *cfgFile =
								new llvm::raw_fd_ostream(fileno(cfgFILE), false, true);

							WriteGraph(*cfgFile, (const Function*)&F, true);
							fflush(cfgFILE);
							fclose(cfgFILE);
						}
					}
					if (is_target)
						ftargets << F.getName().str() << "\n";
					fnames << F.getName().str() << "\n";
				}
			}
			bbnames.close();
			bbcalls.close();
			fnames.close();
			ftargets.close();

		} 
	
	else if (is_aflgo){
		
		for (auto &F : M) {

			int distance = -1;
			int number = -1;
			
			for (auto &BB : F) {

				distance = -1;
				number = -1;
			
					TerminatorInst *TI = BB.getTerminator();
					IRBuilder<> Builder(TI);

					std::string bb_name;
					for (auto &I : BB) {

						#ifdef LLVM_OLD_DEBUG_API
							DebugLoc Loc = I.getDebugLoc();
							if (!Loc.isUnknown()) {

								DILocation cDILoc(Loc.getAsMDNode(M.getContext()));
								DILocation oDILoc = cDILoc.getOrigLocation();

								unsigned line = oDILoc.getLineNumber();
								std::string filename = oDILoc.getFilename().str();

								if (filename.empty()) {
									line = cDILoc.getLineNumber();
									filename = cDILoc.getFilename().str();
								}
						#else
								if (DILocation *Loc = I.getDebugLoc()) {

									unsigned line = Loc->getLine();
									std::string filename = Loc->getFilename().str();

									if (filename.empty()) {
										DILocation *oDILoc = Loc->getInlinedAt();
										if (oDILoc) {
											line = oDILoc->getLine();
											filename = oDILoc->getFilename().str();
										}
									}
							#endif /* LLVM_OLD_DEBUG_API */

									if (filename.empty() || line == 0)
										continue;
									std::size_t found = filename.find_last_of("/\\");
									if (found != std::string::npos)
										filename = filename.substr(found + 1);

									bb_name = filename + ":" + std::to_string(line);
									break;

								}

                            }


                            if (!bb_name.empty()) {

                                if (find(basic_blocks.begin(), basic_blocks.end(), bb_name) == basic_blocks.end()) {

									if (is_selective)
										continue;

								} else {

									/* Find distance for BB */

									if (AFL_R(100) < dinst_ratio) {
										std::map<std::string,int>::iterator it;
										for (it = bb_to_dis.begin(); it != bb_to_dis.end(); ++it)
											if (it->first.compare(bb_name) == 0)
												distance = it->second;

									/* DEBUG */
		                            // ACTF("Distance for %s\t: %d", bb_name.c_str(), distance);
									}
		
									/*Find number for BB*/
			 
									if (AFL_R(100) < dinst_ratio) {
										std::map<std::string,int>::iterator itt;
										for (itt = bb_to_number.begin(); itt != bb_to_number.end(); ++itt)
											if (itt->first.compare(bb_name) == 0)
												number = itt->second;
											/* DEBUG */
										//ACTF("Number for %s\t: %d", bb_name.c_str(), number);

									}
								
                                }
                            }

							BasicBlock::iterator IP = BB.getFirstInsertionPt();
							IRBuilder<> IRB(&(*IP));

							if (AFL_R(100) >= inst_ratio) continue;

							/* Make up cur_loc */

							unsigned int cur_loc = AFL_R(MAP_SIZE);

							ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

							/* Load prev_loc */

							LoadInst *PrevLoc = IRB.CreateLoad(AFLPrevLoc);
							PrevLoc->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
							Value *PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt32Ty());

							/* Load SHM pointer */

							LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
							MapPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
							Value *MapPtrIdx =
									IRB.CreateGEP(MapPtr, IRB.CreateXor(PrevLocCasted, CurLoc));

							/* Update bitmap */

							LoadInst *Counter = IRB.CreateLoad(MapPtrIdx);
							Counter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
							Value *Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
							IRB.CreateStore(Incr, MapPtrIdx)
							   ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

							/* Set prev_loc to cur_loc >> 1 */

							StoreInst *Store =
								IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
							Store->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
							
							if (distance >= 0) {

								unsigned int udistance = (unsigned) distance;

								#ifdef __x86_64__
									  IntegerType *LargestType = Int64Ty;
									  ConstantInt *MapDistLoc = ConstantInt::get(LargestType, MAP_SIZE);
									  ConstantInt *MapCntLoc = ConstantInt::get(LargestType, MAP_SIZE + 8);
									  ConstantInt *Distance = ConstantInt::get(LargestType, udistance);
								#else
									  IntegerType *LargestType = Int32Ty;
									  ConstantInt *MapDistLoc = ConstantInt::get(LargestType, MAP_SIZE);
									  ConstantInt *MapCntLoc = ConstantInt::get(LargestType, MAP_SIZE + 4);
									  ConstantInt *Distance = ConstantInt::get(LargestType, udistance);
								#endif

								/* Add distance to shm[MAPSIZE ] */

								Value *MapDistPtr = IRB.CreateGEP(MapPtr, MapDistLoc);
								#ifdef LLVM_OLD_DEBUG_API
									  LoadInst *MapDist = IRB.CreateLoad(MapDistPtr);
									  MapDist->mutateType(LargestType);
								#else
									  LoadInst *MapDist = IRB.CreateLoad(LargestType, MapDistPtr);
								#endif
								MapDist->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
								Value *IncrDist = IRB.CreateAdd(MapDist, Distance);
								IRB.CreateStore(IncrDist, MapDistPtr)
									  ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

								/* Increase count at to shm[MAPSIZE + (4 or 8)] */

								Value *MapCntPtr = IRB.CreateGEP(MapPtr, MapCntLoc);
								#ifdef LLVM_OLD_DEBUG_API
									  LoadInst *MapCnt = IRB.CreateLoad(MapCntPtr);
									  MapCnt->mutateType(LargestType);
								#else
										LoadInst *MapCnt = IRB.CreateLoad(LargestType, MapCntPtr);
								#endif
								MapCnt->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
								Value *IncrCnt = IRB.CreateAdd(MapCnt, ConstantInt::get(LargestType, 1));
								IRB.CreateStore(IncrCnt, MapCntPtr)
									  ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
							}
					
							if (number > 0){
								/* find current basic block's number*/
		
								unsigned int unumber = (unsigned) number;
								ConstantInt *CurLocation = ConstantInt::get(Int32Ty, unumber);
								
								/* find previous basic block's number */
								
								LoadInst *PrevLocation = IRB.CreateLoad(AFLGoPrevLoc);
								PrevLocation->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
								Value *PrevLocationCasted = IRB.CreateZExt(PrevLocation, IRB.getInt32Ty());
								
								/* Load SHM pointer ;Find the location in Shared memory by previous basic block's number and 
								   current basic block's number*/

								LoadInst *MapPointer = IRB.CreateLoad(AFLGoMapPtr);
								MapPointer->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
								Value *MapPointerIdx =
									IRB.CreateGEP(MapPointer, IRB.CreateXor(PrevLocationCasted, CurLocation));
								
								/* Count the number of executions of the branch and stores it to the SHM(MATRIX)*/
								
								LoadInst *BCounter = IRB.CreateLoad(MapPointerIdx);
								BCounter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
								Value *BIncr = IRB.CreateAdd(BCounter, ConstantInt::get(Int8Ty, 1));
								IRB.CreateStore(BIncr, MapPointerIdx)
								   ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
		
		
								 /* Set PrevLocation to CurLocation */

								StoreInst *BStore =
									IRB.CreateStore(ConstantInt::get(Int32Ty, unumber >> 1), AFLGoPrevLoc);
								BStore->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
								
								ACTF(" instrumentation success !!!!!!! ");

							}
                            
                            inst_blocks++;

                    }
				
			}

  /* Say something nice. */

			if (!is_aflgo_preprocessing && !be_quiet) {

				if (!inst_blocks) 
					WARNF("No instrumentation targets found.");
				else 
					OKF("Instrumented %u locations (%s mode, ratio %u%%, dist. ratio %u%%).",
						inst_blocks,
						getenv("AFL_HARDEN")
						? "hardened"
						: ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN"))
						? "ASAN/MSAN" : "non-hardened"),
						inst_ratio, dinst_ratio);

			}

			return true;

		}

}

static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
