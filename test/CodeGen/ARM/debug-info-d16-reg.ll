; RUN: llc < %s | FileCheck %s
; Radar 9309221
; Test dwarf reg no for d16
;CHECK: DW_OP_regx
;CHECK-NEXT: 272

target datalayout = "e-p:32:32:32-i1:8:32-i8:8:32-i16:16:32-i32:32:32-i64:32:64-f32:32:32-f64:32:64-v64:32:64-v128:32:128-a0:0:32-n32"
target triple = "thumbv7-apple-darwin10"

@.str = private unnamed_addr constant [11 x i8] c"%p %lf %c\0A\00", align 4
@.str1 = private unnamed_addr constant [6 x i8] c"point\00", align 4

define i32 @inlineprinter(i8* %ptr, double %val, i8 zeroext %c) nounwind optsize {
entry:
  tail call void @llvm.dbg.value(metadata i8* %ptr, i64 0, metadata !19, metadata !MDExpression()), !dbg !26
  tail call void @llvm.dbg.value(metadata double %val, i64 0, metadata !20, metadata !MDExpression()), !dbg !26
  tail call void @llvm.dbg.value(metadata i8 %c, i64 0, metadata !21, metadata !MDExpression()), !dbg !26
  %0 = zext i8 %c to i32, !dbg !27
  %1 = tail call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([11 x i8], [11 x i8]* @.str, i32 0, i32 0), i8* %ptr, double %val, i32 %0) nounwind, !dbg !27
  ret i32 0, !dbg !29
}

define i32 @printer(i8* %ptr, double %val, i8 zeroext %c) nounwind optsize noinline {
entry:
  tail call void @llvm.dbg.value(metadata i8* %ptr, i64 0, metadata !16, metadata !MDExpression()), !dbg !30
  tail call void @llvm.dbg.value(metadata double %val, i64 0, metadata !17, metadata !MDExpression()), !dbg !30
  tail call void @llvm.dbg.value(metadata i8 %c, i64 0, metadata !18, metadata !MDExpression()), !dbg !30
  %0 = zext i8 %c to i32, !dbg !31
  %1 = tail call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([11 x i8], [11 x i8]* @.str, i32 0, i32 0), i8* %ptr, double %val, i32 %0) nounwind, !dbg !31
  ret i32 0, !dbg !33
}

declare i32 @printf(i8* nocapture, ...) nounwind

declare void @llvm.dbg.value(metadata, i64, metadata, metadata) nounwind readnone

define i32 @main(i32 %argc, i8** nocapture %argv) nounwind optsize {
entry:
  tail call void @llvm.dbg.value(metadata i32 %argc, i64 0, metadata !22, metadata !MDExpression()), !dbg !34
  tail call void @llvm.dbg.value(metadata i8** %argv, i64 0, metadata !23, metadata !MDExpression()), !dbg !34
  %0 = sitofp i32 %argc to double, !dbg !35
  %1 = fadd double %0, 5.555552e+05, !dbg !35
  tail call void @llvm.dbg.value(metadata double %1, i64 0, metadata !24, metadata !MDExpression()), !dbg !35
  %2 = tail call i32 @puts(i8* getelementptr inbounds ([6 x i8], [6 x i8]* @.str1, i32 0, i32 0)) nounwind, !dbg !36
  %3 = getelementptr inbounds i8, i8* bitcast (i32 (i32, i8**)* @main to i8*), i32 %argc, !dbg !37
  %4 = trunc i32 %argc to i8, !dbg !37
  %5 = add i8 %4, 97, !dbg !37
  tail call void @llvm.dbg.value(metadata i8* %3, i64 0, metadata !49, metadata !MDExpression()) nounwind, !dbg !38
  tail call void @llvm.dbg.value(metadata double %1, i64 0, metadata !50, metadata !MDExpression()) nounwind, !dbg !38
  tail call void @llvm.dbg.value(metadata i8 %5, i64 0, metadata !51, metadata !MDExpression()) nounwind, !dbg !38
  %6 = zext i8 %5 to i32, !dbg !39
  %7 = tail call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([11 x i8], [11 x i8]* @.str, i32 0, i32 0), i8* %3, double %1, i32 %6) nounwind, !dbg !39
  %8 = tail call i32 @printer(i8* %3, double %1, i8 zeroext %5) nounwind, !dbg !40
  ret i32 0, !dbg !41
}

declare i32 @puts(i8* nocapture) nounwind

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!48}

!0 = !MDSubprogram(name: "printer", linkageName: "printer", line: 12, isLocal: false, isDefinition: true, virtualIndex: 6, flags: DIFlagPrototyped, isOptimized: true, scopeLine: 12, file: !46, scope: !1, type: !3, function: i32 (i8*, double, i8)* @printer, variables: !43)
!1 = !MDFile(filename: "a.c", directory: "/tmp/")
!2 = !MDCompileUnit(language: DW_LANG_C89, producer: "(LLVM build 00)", isOptimized: true, emissionKind: 1, file: !46, enums: !47, retainedTypes: !47, subprograms: !42, imports:  null)
!3 = !MDSubroutineType(types: !4)
!4 = !{!5, !6, !7, !8}
!5 = !MDBasicType(tag: DW_TAG_base_type, name: "int", size: 32, align: 32, encoding: DW_ATE_signed)
!6 = !MDDerivedType(tag: DW_TAG_pointer_type, size: 32, align: 32, file: !46, scope: !1, baseType: null)
!7 = !MDBasicType(tag: DW_TAG_base_type, name: "double", size: 64, align: 32, encoding: DW_ATE_float)
!8 = !MDBasicType(tag: DW_TAG_base_type, name: "unsigned char", size: 8, align: 8, encoding: DW_ATE_unsigned_char)
!9 = !MDSubprogram(name: "inlineprinter", linkageName: "inlineprinter", line: 5, isLocal: false, isDefinition: true, virtualIndex: 6, flags: DIFlagPrototyped, isOptimized: true, scopeLine: 5, file: !46, scope: !1, type: !3, function: i32 (i8*, double, i8)* @inlineprinter, variables: !44)
!10 = !MDSubprogram(name: "main", linkageName: "main", line: 18, isLocal: false, isDefinition: true, virtualIndex: 6, flags: DIFlagPrototyped, isOptimized: true, scopeLine: 18, file: !46, scope: !1, type: !11, function: i32 (i32, i8**)* @main, variables: !45)
!11 = !MDSubroutineType(types: !12)
!12 = !{!5, !5, !13}
!13 = !MDDerivedType(tag: DW_TAG_pointer_type, size: 32, align: 32, file: !46, scope: !1, baseType: !14)
!14 = !MDDerivedType(tag: DW_TAG_pointer_type, size: 32, align: 32, file: !46, scope: !1, baseType: !15)
!15 = !MDBasicType(tag: DW_TAG_base_type, name: "char", size: 8, align: 8, encoding: DW_ATE_signed_char)
!16 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "ptr", line: 11, arg: 1, scope: !0, file: !1, type: !6)
!17 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "val", line: 11, arg: 2, scope: !0, file: !1, type: !7)
!18 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "c", line: 11, arg: 3, scope: !0, file: !1, type: !8)
!19 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "ptr", line: 4, arg: 1, scope: !9, file: !1, type: !6)
!20 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "val", line: 4, arg: 2, scope: !9, file: !1, type: !7)
!21 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "c", line: 4, arg: 3, scope: !9, file: !1, type: !8)

!49 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "ptr", line: 4, arg: 1, scope: !9, file: !1, type: !6, inlinedAt: !37)
!50 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "val", line: 4, arg: 2, scope: !9, file: !1, type: !7, inlinedAt: !37)
!51 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "c", line: 4, arg: 2, scope: !9, file: !1, type: !8, inlinedAt: !37)

!22 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "argc", line: 17, arg: 0, scope: !10, file: !1, type: !5)
!23 = !MDLocalVariable(tag: DW_TAG_arg_variable, name: "argv", line: 17, arg: 0, scope: !10, file: !1, type: !13)
!24 = !MDLocalVariable(tag: DW_TAG_auto_variable, name: "dval", line: 19, scope: !25, file: !1, type: !7)
!25 = distinct !MDLexicalBlock(line: 18, column: 0, file: !46, scope: !10)
!26 = !MDLocation(line: 4, scope: !9)
!27 = !MDLocation(line: 6, scope: !28)
!28 = distinct !MDLexicalBlock(line: 5, column: 0, file: !46, scope: !9)
!29 = !MDLocation(line: 7, scope: !28)
!30 = !MDLocation(line: 11, scope: !0)
!31 = !MDLocation(line: 13, scope: !32)
!32 = distinct !MDLexicalBlock(line: 12, column: 0, file: !46, scope: !0)
!33 = !MDLocation(line: 14, scope: !32)
!34 = !MDLocation(line: 17, scope: !10)
!35 = !MDLocation(line: 19, scope: !25)
!36 = !MDLocation(line: 20, scope: !25)
!37 = !MDLocation(line: 21, scope: !25)
!38 = !MDLocation(line: 4, scope: !9, inlinedAt: !37)
!39 = !MDLocation(line: 6, scope: !28, inlinedAt: !37)
!40 = !MDLocation(line: 22, scope: !25)
!41 = !MDLocation(line: 23, scope: !25)
!42 = !{!0, !9, !10}
!43 = !{!16, !17, !18}
!44 = !{!19, !20, !21}
!45 = !{!22, !23, !24}
!46 = !MDFile(filename: "a.c", directory: "/tmp/")
!47 = !{}
!48 = !{i32 1, !"Debug Info Version", i32 3}