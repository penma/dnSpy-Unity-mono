	.SPACE $PRIVATE$
	.SUBSPA $DATA$,QUAD=1,ALIGN=8,ACCESS=31
	.SUBSPA $BSS$,QUAD=1,ALIGN=8,ACCESS=31,ZERO,SORT=82
	.SPACE $TEXT$
	.SUBSPA $LIT$,QUAD=0,ALIGN=8,ACCESS=44
	.SUBSPA $CODE$,QUAD=0,ALIGN=8,ACCESS=44,CODE_ONLY
	.IMPORT $global$,DATA
	.IMPORT $$dyncall,MILLICODE
	.SPACE $TEXT$
	.SUBSPA $CODE$

	.align 4
	.EXPORT GC_test_and_clear,ENTRY,PRIV_LEV=3,ARGW0=GR,RTNVAL=GR
GC_test_and_clear
	.PROC
	.CALLINFO FRAME=0,NO_CALLS
	.ENTRY
	ldcw,co (%r26),%r28
	bv,n 0(%r2)
	.EXIT
	.PROCEND
