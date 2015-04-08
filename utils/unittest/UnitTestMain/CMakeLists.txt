#add_library(testme TestMain.cpp)
#target_link_libraries(testme gtest)

add_llvm_library(gtest_main
  TestMain.cpp
  LINK_LIBS
  gtest_llvm
  LLVMSupport # Depends on llvm::cl
)
