// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "EEMmu.h"

#include <algorithm>
#include <array>
#include <bit>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#include <immintrin.h>
#include <cpuinfo.h>
#include "common/emitter/x86types.h"
#endif

namespace EEMmu
{
	static u32 s_translation_generation = 1;
	static std::array<u32, TLB_ENTRY_COUNT> s_tlb_entry_generations = []() {
		std::array<u32, TLB_ENTRY_COUNT> generations;
		generations.fill(1);
		return generations;
	}();

	static constexpr u32 STATUS_KSU_MASK = 0x18;
	static constexpr u32 STATUS_ERL = 0x04;
	static constexpr u32 STATUS_EXL = 0x02;
	static constexpr u32 STATUS_BEV = 1U << 22;
	static constexpr u32 CAUSE_EXCCODE_MASK = 0x7c;
	static constexpr u32 CAUSE_BD = 1U << 31;
	static constexpr u32 ENTRY_VPN2_MASK = 0x7ffff;

	static bool IsValidPageMask(u32 mask)
	{
		switch (mask)
		{
			case 0x000:
			case 0x003:
			case 0x00f:
			case 0x03f:
			case 0x0ff:
			case 0x3ff:
			case 0xfff:
				return true;

			default:
				return false;
		}
	}

	static bool IsValidCacheMode(u8 cache_mode)
	{
		return cache_mode == 2 || cache_mode == 3 || cache_mode == 7;
	}

	static u32 GetEntryVirtualBase(const tlbs& entry)
	{
		return (entry.EntryHi.VPN2 & ENTRY_VPN2_MASK) << 13;
	}

	static bool MatchesASID(const tlbs& entry, u8 asid)
	{
		return entry.isGlobal() || entry.EntryHi.ASID == asid;
	}

	static bool MatchesNormalEntry(const tlbs& entry, u32 vaddr)
	{
		const u32 mask = entry.Mask() & 0xfff;
		const u32 comparison_mask = ENTRY_VPN2_MASK & ~mask;
		return ((vaddr >> 13) & comparison_mask) == (entry.EntryHi.VPN2 & comparison_mask);
	}

	static bool IsValidScratchpadEntry(const tlbs& entry)
	{
		return entry.Mask() == 0 && (GetEntryVirtualBase(entry) & 0x3fff) == 0 &&
		       entry.EntryLo0.V == entry.EntryLo1.V && entry.EntryLo0.D == entry.EntryLo1.D;
	}

	static bool MatchesScratchpadEntry(const tlbs& entry, u32 vaddr)
	{
		return ((vaddr ^ GetEntryVirtualBase(entry)) & ~0x3fffU) == 0;
	}

	// Match table for the live tlb[] array: entry i matches vaddr when
	// (vaddr & vaddr_mask[i]) == vaddr_base[i] and ((asid ^ asid_value[i]) & asid_mask[i]) == 0.
	// Rebuilt lazily for entries flagged by InvalidateTLBEntry()/InvalidateTranslations().
	static bool s_fast_lookup_enabled = false;
	static constexpr u64 ALL_TLB_ENTRIES = (1ULL << TLB_ENTRY_COUNT) - 1;
	static u64 s_fast_lookup_dirty = ALL_TLB_ENTRIES;
	alignas(64) static u32 s_fast_vaddr_mask[TLB_ENTRY_COUNT];
	alignas(64) static u32 s_fast_vaddr_base[TLB_ENTRY_COUNT];
	alignas(16) static u32 s_fast_asid_mask[TLB_ENTRY_COUNT];
	alignas(16) static u32 s_fast_asid_value[TLB_ENTRY_COUNT];
	// AVX-512 path: one ASID byte per entry plus a bit per global entry, so the ASID test is a single
	// VPCMPEQB into a 64-bit mask. Needs proper testing against the SSE path on real workloads.
	alignas(64) static u8 s_fast_asid_byte[64] = {};
	static u64 s_fast_global_bits = 0;
	// Per-ASID bitset of non-global entries (256 * 8 = 2 KiB), updated when an entry is rebuilt, so a lookup
	// does one load instead of a 64-byte compare. Needs proper testing against the SSE path on real workloads.
	alignas(64) static u64 s_fast_asid_bits[256] = {};
	static bool s_fast_use_avx512 = false;
	static bool s_fast_use_avx2 = false;

	static void RebuildFastLookupEntry(size_t index, const tlbs& entry)
	{
		if (entry.isSPR() && IsValidScratchpadEntry(entry))
		{
			s_fast_vaddr_mask[index] = ~0x3fffU;
			s_fast_vaddr_base[index] = GetEntryVirtualBase(entry) & ~0x3fffU;
		}
		else
		{
			const u32 comparison_mask = ENTRY_VPN2_MASK & ~(entry.Mask() & 0xfff);
			s_fast_vaddr_mask[index] = comparison_mask << 13;
			s_fast_vaddr_base[index] = (entry.EntryHi.VPN2 & comparison_mask) << 13;
		}
		s_fast_asid_mask[index] = entry.isGlobal() ? 0 : 0xff;
		s_fast_asid_value[index] = entry.EntryHi.ASID;
		s_fast_asid_bits[s_fast_asid_byte[index]] &= ~(1ULL << index);
		s_fast_asid_byte[index] = static_cast<u8>(entry.EntryHi.ASID);
		if (entry.isGlobal())
		{
			s_fast_global_bits |= 1ULL << index;
		}
		else
		{
			s_fast_global_bits &= ~(1ULL << index);
			s_fast_asid_bits[s_fast_asid_byte[index]] |= 1ULL << index;
		}
	}

#if defined(_M_X64) || defined(__x86_64__)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((target("avx512f,avx512vl,avx512bw,avx512dq")))
#endif
	static u64 FastLookupMatchesAVX512(u32 vaddr, u64 asid_match)
	{
		const __m512i v_vaddr = _mm512_set1_epi32(static_cast<int>(vaddr));
		u64 matches = 0;
		for (size_t i = 0; i < TLB_ENTRY_COUNT; i += 16)
		{
			const __m512i mask = _mm512_load_si512(&s_fast_vaddr_mask[i]);
			const __m512i base = _mm512_load_si512(&s_fast_vaddr_base[i]);
			const __mmask16 hit = _mm512_mask_cmpeq_epi32_mask(static_cast<__mmask16>(asid_match >> i),
				_mm512_and_si512(v_vaddr, mask), base);
			matches |= static_cast<u64>(hit) << i;
		}
		return matches;
	}
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((target("avx2")))
#endif
	static u64 FastLookupMatchesAVX2(u32 vaddr, u64 asid_match)
	{
		const __m256i v_vaddr = _mm256_set1_epi32(static_cast<int>(vaddr));
		u64 matches = 0;
		for (size_t i = 0; i < TLB_ENTRY_COUNT; i += 8)
		{
			const __m256i mask = _mm256_load_si256(reinterpret_cast<const __m256i*>(&s_fast_vaddr_mask[i]));
			const __m256i base = _mm256_load_si256(reinterpret_cast<const __m256i*>(&s_fast_vaddr_base[i]));
			const int bits = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(v_vaddr, mask), base)));
			matches |= static_cast<u64>(bits) << i;
		}
		return matches & asid_match;
	}
#endif

	// Bit i set when tlb entry i matches; same predicate as the MatchesASID/Matches*Entry loop.
	static u64 FastLookupMatches(const tlbs* entries, u32 vaddr, u8 asid)
	{
		if (s_fast_lookup_dirty)
		{
			for (u64 dirty = s_fast_lookup_dirty; dirty; dirty &= dirty - 1)
			{
				const size_t index = static_cast<size_t>(std::countr_zero(dirty));
				RebuildFastLookupEntry(index, entries[index]);
			}
			s_fast_lookup_dirty = 0;
		}

		u64 matches = 0;
#if defined(_M_X64) || defined(__x86_64__)
		const u64 asid_match = s_fast_asid_bits[asid] | s_fast_global_bits;
		if (s_fast_use_avx512)
			return FastLookupMatchesAVX512(vaddr, asid_match);
		if (s_fast_use_avx2)
			return FastLookupMatchesAVX2(vaddr, asid_match);
		const __m128i v_vaddr = _mm_set1_epi32(static_cast<int>(vaddr));
		const __m128i v_asid = _mm_set1_epi32(asid);
		const __m128i zero = _mm_setzero_si128();
		for (size_t i = 0; i < TLB_ENTRY_COUNT; i += 4)
		{
			const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i*>(&s_fast_vaddr_mask[i]));
			const __m128i base = _mm_load_si128(reinterpret_cast<const __m128i*>(&s_fast_vaddr_base[i]));
			const __m128i amask = _mm_load_si128(reinterpret_cast<const __m128i*>(&s_fast_asid_mask[i]));
			const __m128i avalue = _mm_load_si128(reinterpret_cast<const __m128i*>(&s_fast_asid_value[i]));
			const __m128i vaddr_hit = _mm_cmpeq_epi32(_mm_and_si128(v_vaddr, mask), base);
			const __m128i asid_hit = _mm_cmpeq_epi32(_mm_and_si128(_mm_xor_si128(v_asid, avalue), amask), zero);
			const int bits = _mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(vaddr_hit, asid_hit)));
			matches |= static_cast<u64>(bits) << i;
		}
#else
		for (size_t i = 0; i < TLB_ENTRY_COUNT; i++)
		{
			const bool hit = (vaddr & s_fast_vaddr_mask[i]) == s_fast_vaddr_base[i] &&
			                 ((asid ^ s_fast_asid_value[i]) & s_fast_asid_mask[i]) == 0;
			matches |= static_cast<u64>(hit) << i;
		}
#endif
		return matches;
	}

	void EnableFastTLBLookup(bool enable)
	{
		s_fast_lookup_enabled = enable;
#if defined(_M_X64) || defined(__x86_64__)
		s_fast_use_avx512 = x86Emitter::avx512.HasCore();
		s_fast_use_avx2 = cpuinfo_has_x86_avx2();
#endif
		s_fast_lookup_dirty = ALL_TLB_ENTRIES;
	}

	static TranslationResult MakeDirectResult(u32 paddr, u8 cache_mode, Warning warnings)
	{
		TranslationResult result;
		result.paddr = paddr;
		result.cache_mode = cache_mode;
		result.warnings = warnings;
		if (!IsValidCacheMode(cache_mode))
			result.warnings |= Warning::ReservedCacheMode;
		return result;
	}

	TranslationResult TranslateAddress(const TranslationContext& context, u32 vaddr, AccessType access_type)
	{
		Warning warnings = Warning::None;
		const bool erl = (context.status & STATUS_ERL) != 0;
		const bool exception_level = (context.status & (STATUS_EXL | STATUS_ERL)) != 0;
		const u32 ksu = (context.status & STATUS_KSU_MASK) >> 3;
		if (ksu == 3)
			warnings |= Warning::ReservedKSU;

		enum class OperatingMode
		{
			Kernel,
			Supervisor,
			User,
		};

		OperatingMode mode;
		if (exception_level || ksu == 0)
		{
			mode = OperatingMode::Kernel;
		}
		else if (ksu == 1)
		{
			mode = OperatingMode::Supervisor;
		}
		else if (ksu == 2)
		{
			mode = OperatingMode::User;
		}
		else
		{
			// Reserved KSU behavior is undefined. Kernel is the deterministic compatibility fallback;
			// needs proper testing on hardware-sensitive software.
			mode = OperatingMode::Kernel;
		}

		if (mode == OperatingMode::User && vaddr >= 0x80000000)
		{
			TranslationResult result;
			result.fault = Fault::AddressError;
			result.warnings = warnings;
			return result;
		}

		if (mode == OperatingMode::Supervisor &&
			(vaddr >= 0x80000000 && (vaddr < 0xc0000000 || vaddr >= 0xe0000000)))
		{
			TranslationResult result;
			result.fault = Fault::AddressError;
			result.warnings = warnings;
			return result;
		}

		if (mode == OperatingMode::Kernel)
		{
			if (erl && vaddr < 0x80000000)
				return MakeDirectResult(vaddr, 2, warnings);
			if (vaddr >= 0x80000000 && vaddr < 0xa0000000)
				return MakeDirectResult(vaddr - 0x80000000, context.config & 0x7, warnings);
			if (vaddr >= 0xa0000000 && vaddr < 0xc0000000)
				return MakeDirectResult(vaddr - 0xa0000000, 2, warnings);
		}

		const tlbs* matched_entry = nullptr;
		bool matched_scratchpad = false;
		const size_t entry_count = context.tlb_entries ? std::min(context.tlb_entry_count, TLB_ENTRY_COUNT) : 0;
		if (s_fast_lookup_enabled && context.tlb_entries == tlb && entry_count == TLB_ENTRY_COUNT)
		{
			const u64 matches = FastLookupMatches(context.tlb_entries, vaddr, context.asid);
			if (matches)
			{
				const tlbs& entry = context.tlb_entries[std::countr_zero(matches)];
				matched_entry = &entry;
				matched_scratchpad = entry.isSPR() && IsValidScratchpadEntry(entry);
				// Multiple matching entries are architecturally undefined; lowest index wins.
				if (matches & (matches - 1))
					warnings |= Warning::MultipleMatch;
				if (!IsValidPageMask(entry.Mask()))
					warnings |= Warning::InvalidPageMask;
				if (entry.isSPR() && !matched_scratchpad)
					warnings |= Warning::InvalidScratchpad;
			}
		}
		else for (size_t i = 0; i < entry_count; i++)
		{
			const tlbs& entry = context.tlb_entries[i];
			if (!MatchesASID(entry, context.asid))
				continue;

			const bool valid_scratchpad = entry.isSPR() && IsValidScratchpadEntry(entry);
			const bool match = valid_scratchpad ? MatchesScratchpadEntry(entry, vaddr) : MatchesNormalEntry(entry, vaddr);
			if (!match)
				continue;

			if (matched_entry)
			{
				// Multiple matching entries are architecturally undefined. Lowest index wins so that
				// results remain deterministic; needs proper testing.
				warnings |= Warning::MultipleMatch;
				continue;
			}

			matched_entry = &entry;
			matched_scratchpad = valid_scratchpad;
			if (!IsValidPageMask(entry.Mask()))
				warnings |= Warning::InvalidPageMask;
			if (entry.isSPR() && !valid_scratchpad)
			{
				// Invalid SPRAM mappings are undefined. Fall back to normal TLB interpretation rather
				// than exposing scratchpad through a malformed entry; needs proper testing.
				warnings |= Warning::InvalidScratchpad;
			}
		}

		if (!matched_entry)
		{
			TranslationResult result;
			result.fault = Fault::Refill;
			result.warnings = warnings;
			return result;
		}

		TranslationResult result;
		result.matched_tlb_index = static_cast<int>(matched_entry - context.tlb_entries);
		result.warnings = warnings;

		if (matched_scratchpad)
		{
			if (!matched_entry->EntryLo0.V)
			{
				result.fault = Fault::Invalid;
				return result;
			}
			if (access_type == AccessType::Store && !matched_entry->EntryLo0.D)
			{
				result.fault = Fault::Modified;
				return result;
			}

			result.target = Target::Scratchpad;
			result.scratch_offset = vaddr & 0x3fff;
			result.cache_mode = 2;
			return result;
		}

		const u32 mask = matched_entry->Mask() & 0xfff;
		const u32 page_offset_mask = (mask << 12) | 0xfff;
		const u32 odd_page_bit = page_offset_mask + 1;
		const EntryLo_t& entry_lo = (vaddr & odd_page_bit) ? matched_entry->EntryLo1 : matched_entry->EntryLo0;
		result.cache_mode = entry_lo.C;

		if (!entry_lo.V)
		{
			result.fault = Fault::Invalid;
			return result;
		}
		if (access_type == AccessType::Store && !entry_lo.D)
		{
			result.fault = Fault::Modified;
			return result;
		}

		result.paddr = ((entry_lo.PFN & ~mask) << 12) | (vaddr & page_offset_mask);
		if (!IsValidCacheMode(result.cache_mode))
			result.warnings |= Warning::ReservedCacheMode;
		return result;
	}

	ExceptionResult BuildException(const ExceptionRegisters& registers, const ExceptionRequest& request)
	{
		ExceptionResult result = {registers, 0, true};
		const bool nested_exception = (registers.status & STATUS_EXL) != 0;
		const bool store = request.access_type == AccessType::Store;
		const bool bus_error = request.fault == Fault::BusError;

		// EE bus errors are external exceptions and are masked while EXL or ERL is set.
		if (bus_error && (registers.status & (STATUS_EXL | STATUS_ERL)) != 0)
		{
			result.taken = false;
			return result;
		}

		u32 exception_code;
		switch (request.fault)
		{
			case Fault::Modified:
				exception_code = 1;
				break;
			case Fault::Refill:
			case Fault::Invalid:
				exception_code = store ? 3 : 2;
				break;
			case Fault::AddressError:
				exception_code = store ? 5 : 4;
				break;
			case Fault::BusError:
				exception_code = request.access_type == AccessType::Fetch ? 6 : 7;
				break;
			case Fault::None:
			default:
				exception_code = 0;
				break;
		}

		result.registers.cause = (registers.cause & ~CAUSE_EXCCODE_MASK) | (exception_code << 2);
		if (!bus_error)
			result.registers.bad_vaddr = request.vaddr;

		if (request.fault == Fault::Refill || request.fault == Fault::Invalid || request.fault == Fault::Modified)
		{
			result.registers.context = (registers.context & 0xff80000f) | ((request.vaddr >> 9) & 0x007ffff0);
			result.registers.entry_hi = (request.vaddr & 0xffffe000) | (registers.entry_hi & 0x1fff);
		}

		if (!nested_exception)
		{
			result.registers.status |= STATUS_EXL;
			if (request.branch_delay)
			{
				result.registers.epc = request.fault_pc - 4;
				result.registers.cause |= CAUSE_BD;
			}
			else
			{
				result.registers.epc = request.fault_pc;
				result.registers.cause &= ~CAUSE_BD;
			}
		}

		const bool refill_vector = request.fault == Fault::Refill && !nested_exception;
		const bool bootstrap_vectors = (registers.status & STATUS_BEV) != 0;
		if (bootstrap_vectors)
			result.vector = refill_vector ? 0xbfc00200 : 0xbfc00380;
		else
			result.vector = refill_vector ? 0x80000000 : 0x80000180;

		return result;
	}

	ExceptionResult BuildSynchronousException(
		const ExceptionRegisters& registers, u32 cause_code, u32 fault_pc, bool branch_delay)
	{
		ExceptionResult result = {registers, 0, true};
		const bool nested_exception = (registers.status & STATUS_EXL) != 0;
		result.registers.cause = (registers.cause & ~CAUSE_EXCCODE_MASK) | (cause_code & CAUSE_EXCCODE_MASK);
		if (!nested_exception)
		{
			result.registers.status |= STATUS_EXL;
			if (branch_delay)
			{
				result.registers.epc = fault_pc - 4;
				result.registers.cause |= CAUSE_BD;
			}
			else
			{
				result.registers.epc = fault_pc;
				result.registers.cause &= ~CAUSE_BD;
			}
		}

		result.vector = (registers.status & STATUS_BEV) != 0 ? 0xbfc00380 : 0x80000180;
		return result;
	}

	tlbs BuildTLBEntry(u32 page_mask, u32 entry_hi, u32 entry_lo0, u32 entry_lo1)
	{
		tlbs entry = {};
		entry.PageMask.UL = page_mask & 0x01ffe000;
		entry.EntryHi.UL = entry_hi & ~0x1f00U;
		entry.EntryLo0.UL = entry_lo0 & ~0x7c000000U;
		entry.EntryLo1.UL = entry_lo1 & ~0xfc000000U;

		const u32 mask = entry.Mask();
		entry.EntryHi.VPN2 &= ~mask;
		entry.EntryLo0.PFN &= ~mask;
		entry.EntryLo1.PFN &= ~mask;
		const bool global = entry.EntryLo0.G && entry.EntryLo1.G;
		entry.EntryLo0.G = global;
		entry.EntryLo1.G = global;
		return entry;
	}

	void ReadTLBEntry(const tlbs& entry, u32* page_mask, u32* entry_hi, u32* entry_lo0, u32* entry_lo1)
	{
		const u32 global = entry.isGlobal() ? 1 : 0;
		*page_mask = entry.PageMask.UL & 0x01ffe000;
		*entry_hi = entry.EntryHi.UL & ~((entry.Mask() << 13) | 0x1f00U);
		*entry_lo0 = (entry.EntryLo0.UL & ~0x7c000001U) | global;
		*entry_lo1 = (entry.EntryLo1.UL & ~0xfc000001U) | global;
	}

	ProbeResult ProbeTLB(const tlbs* entries, size_t entry_count, u32 entry_hi)
	{
		ProbeResult result;
		if (!entries)
			return result;

		const u32 vpn2 = entry_hi >> 13;
		const u8 asid = static_cast<u8>(entry_hi);
		for (size_t i = 0; i < std::min(entry_count, TLB_ENTRY_COUNT); i++)
		{
			const tlbs& entry = entries[i];
			if (((vpn2 ^ entry.EntryHi.VPN2) & ~entry.Mask()) != 0 || !MatchesASID(entry, asid))
				continue;
			if (result.index < 0)
				result.index = static_cast<int>(i);
			else
				result.warnings |= Warning::MultipleMatch;
		}
		return result;
	}

	u32 AdvanceRandom(u32 random, u32 wired, Warning* warnings)
	{
		if (wired > 47)
		{
			// Wired values above the architectural table are undefined; needs proper testing.
			if (warnings)
				*warnings |= Warning::InvalidWired;
			return 47;
		}
		if (random > 47 || random <= wired)
			return 47;
		return random - 1;
	}

	u32 GetTranslationGeneration()
	{
		return s_translation_generation;
	}

	const u32* GetTranslationGenerationAddress()
	{
		return &s_translation_generation;
	}

	u32 GetTLBEntryGeneration(size_t index)
	{
		return s_tlb_entry_generations[index];
	}

	const u32* GetTLBEntryGenerationAddress(size_t index)
	{
		return &s_tlb_entry_generations[index];
	}

	const u32* GetTLBEntryGenerationBase()
	{
		return s_tlb_entry_generations.data();
	}

	static void AdvanceGeneration(u32& generation)
	{
		// Zero is not special, but avoiding it makes wraparound diagnostics less ambiguous.
		if (++generation == 0)
			generation = 1;
	}

	void InvalidateTLBEntry(size_t index)
	{
		s_fast_lookup_dirty |= 1ULL << index;
		AdvanceGeneration(s_tlb_entry_generations[index]);
		AdvanceGeneration(s_translation_generation);
	}

	void InvalidateTranslations()
	{
		s_fast_lookup_dirty = ALL_TLB_ENTRIES;
		for (u32& generation : s_tlb_entry_generations)
			AdvanceGeneration(generation);
		AdvanceGeneration(s_translation_generation);
	}
} // namespace EEMmu
