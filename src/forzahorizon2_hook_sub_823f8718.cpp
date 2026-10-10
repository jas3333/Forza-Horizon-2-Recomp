// Diagnostic override for sub_823F8718 -- suspected root cause of the
// heap-corruption crash chain seen at the end of
// logs/forzahorizon2_001.log (2026-09-03 run): thread id=8 fails an
// NtCreateFile for "...StorefrontPurchaseHistory" with a garbage drive
// prefix, then immediately AVs writing to guest addresses that don't
// belong to it, ending in BaseHeap::Release failures and a final
// [FATAL] call to guest address 0.
//
// Manual PPC semantic read of this function (not speculated -- traced
// instruction by instruction from the codegen output) shows this is NOT
// really about StorefrontPurchaseHistory specifically. It is a small-block
// pool/heap free routine: takes a critical section at object+1408, buckets
// the freed item by size (r5 < 128 vs boundary checks against
// object+40/44/48), and on the "coalesce with neighbor" path (taken when a
// flag bit at [(r29-16)+5] & 0x08 is set) does a raw doubly-linked-list
// splice using a Flink/Blink pair loaded from [(r29-16)-32] and
// [(r29-16)-28] -- with ZERO validation that those two words are real
// pointers. This is exactly where the real run's crash happened
// (guest pc 0x823F8930/0x823F8934 -- REX_STORE_U32(r10, r11) then
// REX_STORE_U32(r11+4, r10)).
//
// This override does NOT fix anything -- root cause of why the
// coalescing metadata is garbage is not yet known (could be a
// use-after-free/double-free far upstream, unrelated in time to the
// Storefront file open that happened to be running when the corrupted
// region was touched again). It only adds two non-invasive log lines so
// the next captured run tells us which pool object and which item this
// is, without needing to reproduce the crash again blind.
//
// DEFINE_REX_FUNC emits each recompiled body as a weak symbol, so this
// strong `extern "C" REX_FUNC(...)` definition replaces the generated one
// (same pattern already used in forzahorizon2_hook_sub_823f4ee8.cpp etc).
// Body is a byte-for-byte copy of generated/default/forzahorizon2_recomp.151.cpp
// lines 3-461 (the full sub_823F8718), captured 2026-09-03.
#include "forzahorizon2_recomp.h"

#include <rex/system/checkpoint.h>
#include <rex/system/function_dispatcher.h>

#include <cstdlib>

// The POOLFREE diagnostics log every pool free (100k+ lines a minute in
// gameplay); keep them for debugging but off unless FH2_POOLFREE_LOG is set.
static const bool g_poolfree_log = [] {
  const char* v = std::getenv("FH2_POOLFREE_LOG");
  return v && *v && *v != '0';
}();

// Not declared by forzahorizon2_recomp.h itself -- each generated shard
// forward-declares the specific helpers/subs it calls locally (see
// generated/default/forzahorizon2_funcs.151.h).
DECLARE_REX_FUNC(__savegprlr_26);
DECLARE_REX_FUNC(__restgprlr_26);
DECLARE_REX_FUNC(__imp__KeGetCurrentProcessType);
DECLARE_REX_FUNC(__imp__KeBugCheckEx);
DECLARE_REX_FUNC(__imp__RtlEnterCriticalSection);
DECLARE_REX_FUNC(__imp__RtlLeaveCriticalSection);
DECLARE_REX_FUNC(__imp__NtFreeVirtualMemory);
DECLARE_REX_FUNC(sub_823F4EE8);
DECLARE_REX_FUNC(sub_823F89BC);
DECLARE_REX_FUNC(sub_82BF3AE0);
DECLARE_REX_FUNC(sub_82BF4658);

extern "C" REX_FUNC(sub_823F8718) {
	REX_FUNC_PROLOGUE();
	uint32_t rex_entry = 0x823F8718;
	{
		const uint32_t rex_dispatch_address = ctx.dispatch_address;
		ctx.dispatch_address = 0;
		if (rex_entry == 0x823F8718) {
			switch (rex_dispatch_address) {
				case 0x823F8720:
				case 0x823F8758:
				case 0x823F877C:
				case 0x823F87AC:
				case 0x823F87DC:
				case 0x823F888C:
				case 0x823F8918:
				case 0x823F8944:
				case 0x823F896C:
				case 0x823F8988:
					rex_entry = rex_dispatch_address; break;
				default: break;
			}
		}
	}
	rex::system::PollCheckpointBoundary(ctx, rex_entry);
	const uint32_t rex_entry_lr_ = uint32_t(ctx.lr);
	constexpr bool rex_lr_restore_helper_ = false;
	ctx.current_function = 0x823F8718;
	ctx.current_instruction = rex_entry;
	uint32_t ea{};
	switch (rex_entry) {
		case 0x823F8720: goto loc_823F8720;
		case 0x823F8758: goto loc_823F8758;
		case 0x823F877C: goto loc_823F877C;
		case 0x823F87AC: goto loc_823F87AC;
		case 0x823F87DC: goto loc_823F87DC;
		case 0x823F888C: goto loc_823F888C;
		case 0x823F8918: goto loc_823F8918;
		case 0x823F8944: goto loc_823F8944;
		case 0x823F896C: goto loc_823F896C;
		case 0x823F8988: goto loc_823F8988;
		default: break;
	}
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x82c010b0
	ctx.lr = 0x823F8720;
	__savegprlr_26(ctx, base);
loc_823F8720:
	// addi r31,r1,-160
	ctx.r31.s64 = ctx.r1.s64 + -160;
	// stwu r1,-160(r1)
	ctx.current_instruction = 0x823F8724;
	ea = -160 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// mr r30,r3
	ctx.r30.u64 = ctx.r3.u64;
	// mr r28,r4
	ctx.r28.u64 = ctx.r4.u64;
	// mr r29,r5
	ctx.r29.u64 = ctx.r5.u64;
	// DIAGNOSTIC (2026-09-03): every entry into this pool-free routine.
	// obj=pool/heap object (r3), item=block being freed (r4), extra=r5
	// (0 takes the early-return path; non-zero is the size/flags value
	// used for bucket selection), caller_lr=who called us.
	if (g_poolfree_log) REXLOG_WARN(
		"[POOLFREE-823F8718] obj=0x{:08X} item=0x{:08X} extra=0x{:08X} caller_lr=0x{:08X}",
		ctx.r30.u32, ctx.r28.u32, ctx.r29.u32, rex_entry_lr_);
	// lwz r11,20(r3)
	ctx.current_instruction = 0x823F8734;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 20);
	// li r27,1
	ctx.r27.s64 = 1;
	// li r26,0
	ctx.r26.s64 = 0;
	// rlwinm. r11,r11,0,13,13
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x40000;
	ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
	// stw r3,100(r31)
	ctx.current_instruction = 0x823F8744;
	REX_STORE_U32(ctx.r31.u32 + 100, ctx.r3.u32);
	// stw r26,84(r31)
	ctx.current_instruction = 0x823F8748;
	REX_STORE_U32(ctx.r31.u32 + 84, ctx.r26.u32);
	// stw r27,88(r31)
	ctx.current_instruction = 0x823F874C;
	REX_STORE_U32(ctx.r31.u32 + 88, ctx.r27.u32);
	// beq 0x823f877c
	if (ctx.cr0.eq) goto loc_823F877C;
	// bl 0x832f0284
	ctx.lr = 0x823F8758;
	__imp__KeGetCurrentProcessType(ctx, base);
loc_823F8758:
	// lbz r11,379(r30)
	ctx.current_instruction = 0x823F8758;
	ctx.r11.u64 = REX_LOAD_U8(ctx.r30.u32 + 379);
	// cmpw cr6,r11,r3
	ctx.cr6.compare<int32_t>(ctx.r11.s32, ctx.r3.s32, ctx.xer);
	// beq cr6,0x823f877c
	if (ctx.cr6.eq) goto loc_823F877C;
	// mr r7,r29
	ctx.r7.u64 = ctx.r29.u64;
	// li r6,4448
	ctx.r6.s64 = 4448;
	// lwz r5,152(r31)
	ctx.current_instruction = 0x823F876C;
	ctx.r5.u64 = REX_LOAD_U32(ctx.r31.u32 + 152);
	// mr r4,r30
	ctx.r4.u64 = ctx.r30.u64;
	// li r3,244
	ctx.r3.s64 = 244;
	// bl 0x832f12c4
	ctx.lr = 0x823F877C;
	__imp__KeBugCheckEx(ctx, base);
loc_823F877C:
	// cmplwi cr6,r29,0
	ctx.cr6.compare<uint32_t>(ctx.r29.u32, 0, ctx.xer);
	// bne cr6,0x823f878c
	if (!ctx.cr6.eq) goto loc_823F878C;
	// li r3,1
	ctx.r3.s64 = 1;
	// b 0x823f898c
	goto loc_823F898C;
loc_823F878C:
	// lwz r11,24(r30)
	ctx.current_instruction = 0x823F878C;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 24);
	// addi r29,r29,-16
	ctx.r29.s64 = ctx.r29.s64 + -16;
	// or r11,r11,r28
	ctx.r11.u64 = ctx.r11.u64 | ctx.r28.u64;
	// mr r8,r8
	ctx.r8.u64 = ctx.r8.u64;
	// clrlwi. r11,r11,31
	ctx.r11.u64 = ctx.r11.u32 & 0x1;
	ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
	// bne 0x823f87b4
	if (!ctx.cr0.eq) goto loc_823F87B4;
	// lwz r3,1408(r30)
	ctx.current_instruction = 0x823F87A4;
	ctx.r3.u64 = REX_LOAD_U32(ctx.r30.u32 + 1408);
	// bl 0x832f0294
	ctx.lr = 0x823F87AC;
	__imp__RtlEnterCriticalSection(ctx, base);
loc_823F87AC:
	// mr r26,r27
	ctx.r26.u64 = ctx.r27.u64;
	// stw r27,84(r31)
	ctx.current_instruction = 0x823F87B0;
	REX_STORE_U32(ctx.r31.u32 + 84, ctx.r27.u32);
loc_823F87B4:
	// lbz r11,5(r29)
	ctx.current_instruction = 0x823F87B4;
	ctx.r11.u64 = REX_LOAD_U8(ctx.r29.u32 + 5);
	// rlwinm. r11,r11,0,28,28
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x8;
	ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
	// bne 0x823f891c
	if (!ctx.cr0.eq) goto loc_823F891C;
	// lhz r11,0(r29)
	ctx.current_instruction = 0x823F87C0;
	ctx.r11.u64 = REX_LOAD_U16(ctx.r29.u32 + 0);
	// li r6,0
	ctx.r6.s64 = 0;
	// addi r5,r31,80
	ctx.r5.s64 = ctx.r31.s64 + 80;
	// mr r3,r30
	ctx.r3.u64 = ctx.r30.u64;
	// mr r4,r29
	ctx.r4.u64 = ctx.r29.u64;
	// stw r11,80(r31)
	ctx.current_instruction = 0x823F87D4;
	REX_STORE_U32(ctx.r31.u32 + 80, ctx.r11.u32);
	// bl 0x823f4ee8
	ctx.lr = 0x823F87DC;
	sub_823F4EE8(ctx, base);
loc_823F87DC:
	// lwz r5,80(r31)
	ctx.current_instruction = 0x823F87DC;
	ctx.r5.u64 = REX_LOAD_U32(ctx.r31.u32 + 80);
	// mr r4,r3
	ctx.r4.u64 = ctx.r3.u64;
	// cmplwi cr6,r5,128
	ctx.cr6.compare<uint32_t>(ctx.r5.u32, 128, ctx.xer);
	// bge cr6,0x823f8864
	if (!ctx.cr6.lt) goto loc_823F8864;
	// lbz r11,5(r3)
	ctx.current_instruction = 0x823F87EC;
	ctx.r11.u64 = REX_LOAD_U8(ctx.r3.u32 + 5);
	// rlwinm r11,r11,0,27,27
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x10;
	// stb r11,5(r3)
	ctx.current_instruction = 0x823F87F4;
	REX_STORE_U8(ctx.r3.u32 + 5, ctx.r11.u8);
	// lwz r11,80(r31)
	ctx.current_instruction = 0x823F87F8;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 80);
	// clrlwi r11,r11,16
	ctx.r11.u64 = ctx.r11.u32 & 0xFFFF;
	// addi r11,r11,48
	ctx.r11.s64 = ctx.r11.s64 + 48;
	// rlwinm r11,r11,3,0,28
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 3) & 0xFFFFFFF8;
	// add r11,r11,r30
	ctx.r11.u64 = ctx.r11.u64 + ctx.r30.u64;
	// lwz r10,0(r11)
	ctx.current_instruction = 0x823F880C;
	ctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + 0);
	// cmplw cr6,r10,r11
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, ctx.r11.u32, ctx.xer);
	// bne cr6,0x823f883c
	if (!ctx.cr6.eq) goto loc_823F883C;
	// lhz r9,0(r3)
	ctx.current_instruction = 0x823F8818;
	ctx.r9.u64 = REX_LOAD_U16(ctx.r3.u32 + 0);
	// rlwinm r10,r9,27,5,31
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r9.u32 | (ctx.r9.u64 << 32), 27) & 0x7FFFFFF;
	// clrlwi r9,r9,27
	ctx.r9.u64 = ctx.r9.u32 & 0x1F;
	// addi r10,r10,88
	ctx.r10.s64 = ctx.r10.s64 + 88;
	// slw r9,r27,r9
	ctx.r9.u64 = ctx.r9.u8 & 0x20 ? 0 : (ctx.r27.u32 << (ctx.r9.u8 & 0x3F));
	// rlwinm r10,r10,2,0,29
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 2) & 0xFFFFFFFC;
	// lwzx r8,r10,r30
	ctx.current_instruction = 0x823F8830;
	ctx.r8.u64 = REX_LOAD_U32(ctx.r10.u32 + ctx.r30.u32);
	// or r9,r9,r8
	ctx.r9.u64 = ctx.r9.u64 | ctx.r8.u64;
	// stwx r9,r10,r30
	ctx.current_instruction = 0x823F8838;
	REX_STORE_U32(ctx.r10.u32 + ctx.r30.u32, ctx.r9.u32);
loc_823F883C:
	// lwz r9,4(r11)
	ctx.current_instruction = 0x823F883C;
	ctx.r9.u64 = REX_LOAD_U32(ctx.r11.u32 + 4);
	// addi r10,r4,8
	ctx.r10.s64 = ctx.r4.s64 + 8;
	// stw r11,8(r4)
	ctx.current_instruction = 0x823F8844;
	REX_STORE_U32(ctx.r4.u32 + 8, ctx.r11.u32);
	// stw r9,12(r4)
	ctx.current_instruction = 0x823F8848;
	REX_STORE_U32(ctx.r4.u32 + 12, ctx.r9.u32);
	// stw r10,0(r9)
	ctx.current_instruction = 0x823F884C;
	REX_STORE_U32(ctx.r9.u32 + 0, ctx.r10.u32);
	// stw r10,4(r11)
	ctx.current_instruction = 0x823F8850;
	REX_STORE_U32(ctx.r11.u32 + 4, ctx.r10.u32);
	// lwz r11,80(r31)
	ctx.current_instruction = 0x823F8854;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 80);
	// lwz r10,48(r30)
	ctx.current_instruction = 0x823F8858;
	ctx.r10.u64 = REX_LOAD_U32(ctx.r30.u32 + 48);
	// add r11,r10,r11
	ctx.r11.u64 = ctx.r10.u64 + ctx.r11.u64;
	// b 0x823f8908
	goto loc_823F8908;
loc_823F8864:
	// lwz r11,40(r30)
	ctx.current_instruction = 0x823F8864;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 40);
	// cmplw cr6,r5,r11
	ctx.cr6.compare<uint32_t>(ctx.r5.u32, ctx.r11.u32, ctx.xer);
	// blt cr6,0x823f8890
	if (ctx.cr6.lt) goto loc_823F8890;
	// lwz r11,48(r30)
	ctx.current_instruction = 0x823F8870;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 48);
	// lwz r10,44(r30)
	ctx.current_instruction = 0x823F8874;
	ctx.r10.u64 = REX_LOAD_U32(ctx.r30.u32 + 44);
	// add r11,r11,r5
	ctx.r11.u64 = ctx.r11.u64 + ctx.r5.u64;
	// cmplw cr6,r11,r10
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, ctx.r10.u32, ctx.xer);
	// blt cr6,0x823f8890
	if (ctx.cr6.lt) goto loc_823F8890;
	// mr r3,r30
	ctx.r3.u64 = ctx.r30.u64;
	// bl 0x82bf4658
	ctx.lr = 0x823F888C;
	sub_82BF4658(ctx, base);
loc_823F888C:
	// b 0x823f897c
	goto loc_823F897C;
loc_823F8890:
	// cmplwi cr6,r5,61440
	ctx.cr6.compare<uint32_t>(ctx.r5.u32, 61440, ctx.xer);
	// bgt cr6,0x823f8910
	if (ctx.cr6.gt) goto loc_823F8910;
	// lbz r11,5(r4)
	ctx.current_instruction = 0x823F8898;
	ctx.r11.u64 = REX_LOAD_U8(ctx.r4.u32 + 5);
	// addi r10,r30,384
	ctx.r10.s64 = ctx.r30.s64 + 384;
	// rlwinm r11,r11,0,27,27
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x10;
	// stb r11,5(r4)
	ctx.current_instruction = 0x823F88A4;
	REX_STORE_U8(ctx.r4.u32 + 5, ctx.r11.u8);
	// lwz r11,384(r30)
	ctx.current_instruction = 0x823F88A8;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 384);
	// stw r11,92(r31)
	ctx.current_instruction = 0x823F88AC;
	REX_STORE_U32(ctx.r31.u32 + 92, ctx.r11.u32);
loc_823F88B0:
	// cmplw cr6,r10,r11
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, ctx.r11.u32, ctx.xer);
	// beq cr6,0x823f88e4
	if (ctx.cr6.eq) goto loc_823F88E4;
	// lwz r9,80(r31)
	ctx.current_instruction = 0x823F88B8;
	ctx.r9.u64 = REX_LOAD_U32(ctx.r31.u32 + 80);
	// addi r8,r11,-8
	ctx.r8.s64 = ctx.r11.s64 + -8;
	// lhz r7,-8(r11)
	ctx.current_instruction = 0x823F88C0;
	ctx.r7.u64 = REX_LOAD_U16(ctx.r11.u32 + -8);
	// clrlwi r9,r9,16
	ctx.r9.u64 = ctx.r9.u32 & 0xFFFF;
	// stw r8,96(r31)
	ctx.current_instruction = 0x823F88C8;
	REX_STORE_U32(ctx.r31.u32 + 96, ctx.r8.u32);
	// cmplw cr6,r9,r7
	ctx.cr6.compare<uint32_t>(ctx.r9.u32, ctx.r7.u32, ctx.xer);
	// ble cr6,0x823f88e4
	if (!ctx.cr6.gt) goto loc_823F88E4;
	// lwz r11,0(r11)
	ctx.current_instruction = 0x823F88D4;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 0);
	// stw r11,92(r31)
	ctx.current_instruction = 0x823F88D8;
	REX_STORE_U32(ctx.r31.u32 + 92, ctx.r11.u32);
	// mr r8,r8
	ctx.r8.u64 = ctx.r8.u64;
	// b 0x823f88b0
	goto loc_823F88B0;
loc_823F88E4:
	// lwz r9,4(r11)
	ctx.current_instruction = 0x823F88E4;
	ctx.r9.u64 = REX_LOAD_U32(ctx.r11.u32 + 4);
	// addi r10,r4,8
	ctx.r10.s64 = ctx.r4.s64 + 8;
	// stw r11,8(r4)
	ctx.current_instruction = 0x823F88EC;
	REX_STORE_U32(ctx.r4.u32 + 8, ctx.r11.u32);
	// stw r9,12(r4)
	ctx.current_instruction = 0x823F88F0;
	REX_STORE_U32(ctx.r4.u32 + 12, ctx.r9.u32);
	// stw r10,0(r9)
	ctx.current_instruction = 0x823F88F4;
	REX_STORE_U32(ctx.r9.u32 + 0, ctx.r10.u32);
	// stw r10,4(r11)
	ctx.current_instruction = 0x823F88F8;
	REX_STORE_U32(ctx.r11.u32 + 4, ctx.r10.u32);
	// lwz r10,48(r30)
	ctx.current_instruction = 0x823F88FC;
	ctx.r10.u64 = REX_LOAD_U32(ctx.r30.u32 + 48);
	// lwz r11,80(r31)
	ctx.current_instruction = 0x823F8900;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 80);
	// add r11,r11,r10
	ctx.r11.u64 = ctx.r11.u64 + ctx.r10.u64;
loc_823F8908:
	// stw r11,48(r30)
	ctx.current_instruction = 0x823F8908;
	REX_STORE_U32(ctx.r30.u32 + 48, ctx.r11.u32);
	// b 0x823f897c
	goto loc_823F897C;
loc_823F8910:
	// mr r3,r30
	ctx.r3.u64 = ctx.r30.u64;
	// bl 0x82bf3ae0
	ctx.lr = 0x823F8918;
	sub_82BF3AE0(ctx, base);
loc_823F8918:
	// b 0x823f897c
	goto loc_823F897C;
loc_823F891C:
	// addi r11,r29,-32
	ctx.r11.s64 = ctx.r29.s64 + -32;
	// cmplwi cr6,r26,0
	ctx.cr6.compare<uint32_t>(ctx.r26.u32, 0, ctx.xer);
	// stw r11,96(r31)
	ctx.current_instruction = 0x823F8924;
	REX_STORE_U32(ctx.r31.u32 + 96, ctx.r11.u32);
	// lwz r11,-32(r29)
	ctx.current_instruction = 0x823F8928;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + -32);
	// lwz r10,-28(r29)
	ctx.current_instruction = 0x823F892C;
	ctx.r10.u64 = REX_LOAD_U32(ctx.r29.u32 + -28);
	// DIAGNOSTIC (2026-09-03): about to splice a Flink/Blink pair read
	// straight out of [r29-32]/[r29-28] with no validation. r29 here is
	// already (orig_r29 - 16) from loc_823F878C. This is the exact path
	// that crashed at pc 0x823F8930/0x823F8934 in the captured run. Log
	// everything needed to identify the pool object and item before we
	// touch memory, so a live capture tells us the real values without
	// needing the crash to happen again blind.
	if (g_poolfree_log) REXLOG_WARN(
		"[POOLFREE-COALESCE-823F8718] pool_obj=0x{:08X} item=0x{:08X} "
		"adj_r29=0x{:08X} flink_candidate=0x{:08X} blink_candidate=0x{:08X}",
		ctx.r30.u32, ctx.r28.u32, ctx.r29.u32, ctx.r11.u32, ctx.r10.u32);
	// stw r11,0(r10)
	ctx.current_instruction = 0x823F8930;
	REX_STORE_U32(ctx.r10.u32 + 0, ctx.r11.u32);
	// stw r10,4(r11)
	ctx.current_instruction = 0x823F8934;
	REX_STORE_U32(ctx.r11.u32 + 4, ctx.r10.u32);
	// beq cr6,0x823f894c
	if (ctx.cr6.eq) goto loc_823F894C;
	// lwz r3,1408(r30)
	ctx.current_instruction = 0x823F893C;
	ctx.r3.u64 = REX_LOAD_U32(ctx.r30.u32 + 1408);
	// bl 0x832f02a4
	ctx.lr = 0x823F8944;
	__imp__RtlLeaveCriticalSection(ctx, base);
loc_823F8944:
	// li r26,0
	ctx.r26.s64 = 0;
	// stw r26,84(r31)
	ctx.current_instruction = 0x823F8948;
	REX_STORE_U32(ctx.r31.u32 + 84, ctx.r26.u32);
loc_823F894C:
	// li r11,0
	ctx.r11.s64 = 0;
	// lis r5,0
	ctx.r5.s64 = 0;
	// stw r11,80(r31)
	ctx.current_instruction = 0x823F8954;
	REX_STORE_U32(ctx.r31.u32 + 80, ctx.r11.u32);
	// addi r4,r31,80
	ctx.r4.s64 = ctx.r31.s64 + 80;
	// ori r5,r5,32768
	ctx.r5.u64 = ctx.r5.u64 | 32768;
	// lwz r6,1424(r30)
	ctx.current_instruction = 0x823F8960;
	ctx.r6.u64 = REX_LOAD_U32(ctx.r30.u32 + 1424);
	// addi r3,r31,96
	ctx.r3.s64 = ctx.r31.s64 + 96;
	// bl 0x832f12d4
	ctx.lr = 0x823F896C;
	__imp__NtFreeVirtualMemory(ctx, base);
loc_823F896C:
	// rlwinm r11,r3,1,31,31
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r3.u32 | (ctx.r3.u64 << 32), 1) & 0x1;
	// addi r11,r11,-1
	ctx.r11.s64 = ctx.r11.s64 + -1;
	// and r11,r11,r27
	ctx.r11.u64 = ctx.r11.u64 & ctx.r27.u64;
	// stw r11,88(r31)
	ctx.current_instruction = 0x823F8978;
	REX_STORE_U32(ctx.r31.u32 + 88, ctx.r11.u32);
loc_823F897C:
	// mr r8,r8
	ctx.r8.u64 = ctx.r8.u64;
	// addi r12,r31,160
	ctx.r12.s64 = ctx.r31.s64 + 160;
	// bl 0x823f89bc
	ctx.lr = 0x823F8988;
	sub_823F89BC(ctx, base);
loc_823F8988:
	// lwz r3,88(r31)
	ctx.current_instruction = 0x823F8988;
	ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 88);
loc_823F898C:
	// addi r1,r31,160
	ctx.r1.s64 = ctx.r31.s64 + 160;
	// b 0x82c01100
	__restgprlr_26(ctx, base);
	return;
}
