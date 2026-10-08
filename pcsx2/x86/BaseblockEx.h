// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <vector>

#include "common/Assertions.h"

// Every potential jump point in the PS2's addressable memory has a BASEBLOCK
// associated with it. So that means a BASEBLOCK for every 4 bytes of PS2
// addressable memory.  Yay!
struct BASEBLOCK
{
	uptr m_pFnptr;

	__inline uptr GetFnptr() const { return m_pFnptr; }
	void __inline SetFnptr(uptr ptr) { m_pFnptr = ptr; }
};

// extra block info (only valid for start of fn)
struct BASEBLOCKEX
{
	uptr fnptr;
	u32 startpc;
	u32 size;    // The size in dwords (equivalent to the number of instructions)
	u32 x86size; // The size in byte of the translated x86 instructions

#ifdef PCSX2_DEVBUILD
	// Could be useful to instrument the block
	//u32 visited; // number of times called
	//u64 ltime; // regs it assumes to have set already
#endif
};

// Sorted-by-startpc container of BASEBLOCKEX with array-like indexing. It used to be one flat array,
// so every insert and discard memmove'd the whole tail (Full TLB does that thousands of times a
// second). Entries now live in chunks of at most CHUNK_CAP; an edit moves at most one chunk, and
// entries in other chunks keep their address. `starts` holds the global index of each chunk's
// first entry so operator[](idx) stays a binary search over a few dozen to a few hundred chunks.
// Needs proper testing under Full TLB load.
class BaseBlockArray
{
	static constexpr u32 CHUNK_CAP = 256;

	struct Chunk
	{
		u32 count;
		BASEBLOCKEX items[CHUNK_CAP];
	};

	std::vector<std::unique_ptr<Chunk>> chunks;
	std::vector<s32> starts;
	s32 _Size;

	// Last chunk whose first startpc is <= startpc (chunk 0 when startpc is below everything).
	__fi size_t findChunk(u32 startpc) const
	{
		size_t lo = 0, hi = chunks.size();
		while (hi - lo > 1)
		{
			const size_t mid = (lo + hi) >> 1;
			if (chunks[mid]->items[0].startpc > startpc)
				hi = mid;
			else
				lo = mid;
		}
		return lo;
	}

	__fi size_t findChunkByIndex(s32 idx) const
	{
		size_t lo = 0, hi = chunks.size();
		while (hi - lo > 1)
		{
			const size_t mid = (lo + hi) >> 1;
			if (starts[mid] > idx)
				hi = mid;
			else
				lo = mid;
		}
		return lo;
	}

	void renumber(size_t from)
	{
		s32 pos = from ? starts[from - 1] + static_cast<s32>(chunks[from - 1]->count) : 0;
		for (size_t c = from; c < chunks.size(); c++)
		{
			starts[c] = pos;
			pos += static_cast<s32>(chunks[c]->count);
		}
	}

public:
	BaseBlockArray(s32 size)
		: _Size(0)
	{
		chunks.reserve(static_cast<size_t>(size) / (CHUNK_CAP / 2) + 1);
		starts.reserve(chunks.capacity());
	}

	BASEBLOCKEX* insert(u32 startpc, uptr fnptr)
	{
		if (chunks.empty())
		{
			chunks.push_back(std::make_unique<Chunk>());
			chunks[0]->count = 0;
			starts.push_back(0);
		}

		size_t c = findChunk(startpc);
		if (chunks[c]->count == CHUNK_CAP)
		{
			// Split in half; the upper half becomes a new chunk right after.
			auto upper = std::make_unique<Chunk>();
			const u32 half = CHUNK_CAP / 2;
			memcpy(upper->items, chunks[c]->items + half, (CHUNK_CAP - half) * sizeof(BASEBLOCKEX));
			upper->count = CHUNK_CAP - half;
			chunks[c]->count = half;
			chunks.insert(chunks.begin() + c + 1, std::move(upper));
			starts.insert(starts.begin() + c + 1, 0);
			renumber(c + 1);
			if (chunks[c + 1]->items[0].startpc <= startpc)
				c++;
		}

		Chunk& chunk = *chunks[c];
		u32 imin = 0, imax = chunk.count;
		while (imin < imax)
		{
			const u32 imid = (imin + imax) >> 1;
			if (chunk.items[imid].startpc > startpc)
				imax = imid;
			else
				imin = imid + 1;
		}

		pxAssert(imin == chunk.count || chunk.items[imin].startpc > startpc);

		if (imin < chunk.count)
			memmove(chunk.items + imin + 1, chunk.items + imin, (chunk.count - imin) * sizeof(BASEBLOCKEX));

		memset(&chunk.items[imin], 0, sizeof(BASEBLOCKEX));
		chunk.items[imin].startpc = startpc;
		chunk.items[imin].fnptr = fnptr;
		chunk.count++;
		for (size_t i = c + 1; i < starts.size(); i++)
			starts[i]++;

		_Size++;
		return &chunk.items[imin];
	}

	// Index of the last entry with startpc <= the argument, 0 if there is none, -1 when empty.
	int lastIndex(u32 startpc) const
	{
		if (_Size == 0)
			return -1;

		const size_t c = findChunk(startpc);
		const Chunk& chunk = *chunks[c];
		u32 imin = 0, imax = chunk.count - 1;
		while (imin != imax)
		{
			const u32 imid = (imin + imax + 1) >> 1;
			if (chunk.items[imid].startpc > startpc)
				imax = imid - 1;
			else
				imin = imid;
		}
		return starts[c] + static_cast<s32>(imin);
	}

	__fi BASEBLOCKEX& operator[](int idx) const
	{
		const size_t c = findChunkByIndex(idx);
		return chunks[c]->items[idx - starts[c]];
	}

	void clear()
	{
		chunks.clear();
		starts.clear();
		_Size = 0;
	}

	__fi u32 size() const
	{
		return _Size;
	}

	// Removes entries [first, last).
	void erase(s32 first, s32 last)
	{
		if (first >= last)
			return;

		const size_t c0 = findChunkByIndex(first);
		const size_t c1 = findChunkByIndex(last - 1);
		const u32 off0 = first - starts[c0];
		const u32 off1 = (last - 1) - starts[c1]; // inclusive

		if (c0 == c1)
		{
			Chunk& chunk = *chunks[c0];
			memmove(chunk.items + off0, chunk.items + off1 + 1, (chunk.count - off1 - 1) * sizeof(BASEBLOCKEX));
			chunk.count -= off1 - off0 + 1;
		}
		else
		{
			Chunk& head = *chunks[c0];
			Chunk& tail = *chunks[c1];
			head.count = off0;
			memmove(tail.items, tail.items + off1 + 1, (tail.count - off1 - 1) * sizeof(BASEBLOCKEX));
			tail.count -= off1 + 1;
			// Chunks strictly between are dropped whole.
			chunks.erase(chunks.begin() + c0 + 1, chunks.begin() + c1);
			starts.erase(starts.begin() + c0 + 1, starts.begin() + c1);
		}
		_Size -= last - first;

		// Drop chunks which became empty, and fold a small chunk into its right neighbour.
		for (size_t c = std::min(c0 + 2, chunks.size()); c-- > c0;)
		{
			if (chunks[c]->count == 0)
			{
				chunks.erase(chunks.begin() + c);
				starts.erase(starts.begin() + c);
			}
		}
		size_t c = c0 < chunks.size() ? c0 : chunks.size() - 1;
		if (!chunks.empty() && c + 1 < chunks.size() && chunks[c]->count + chunks[c + 1]->count <= CHUNK_CAP / 2)
		{
			Chunk& dst = *chunks[c];
			Chunk& src = *chunks[c + 1];
			memcpy(dst.items + dst.count, src.items, src.count * sizeof(BASEBLOCKEX));
			dst.count += src.count;
			chunks.erase(chunks.begin() + c + 1);
			starts.erase(starts.begin() + c + 1);
		}
		renumber(c0 < chunks.size() ? c0 : chunks.size());
	}
};

class BaseBlocks
{
protected:
	typedef std::multimap<u32, uptr>::iterator linkiter_t;

	// switch to a hash map later?
	std::multimap<u32, uptr> links;
	// Reverse index of `links`: patch site (jumpptr) -> target pc. Lets Remove() find the
	// outgoing links of the discarded code without walking every link in the JIT.
	std::map<uptr, u32> link_sites;
	uptr recompiler;
	BaseBlockArray blocks;

public:
	BaseBlocks()
		: recompiler(0)
		, blocks(0x4000)
	{
	}

	void SetJITCompile(const void *recompiler_)
	{
		recompiler = reinterpret_cast<uptr>(recompiler_);
	}

	BASEBLOCKEX* New(u32 startpc, uptr fnptr);
	int LastIndex(u32 startpc) const;
	//BASEBLOCKEX* GetByX86(uptr ip);

	__fi int Index(u32 startpc) const
	{
		int idx = LastIndex(startpc);

		if ((idx == -1) || (startpc < blocks[idx].startpc) ||
			((blocks[idx].size) && (startpc >= blocks[idx].startpc + blocks[idx].size * 4)))
			return -1;
		else
			return idx;
	}

	__fi BASEBLOCKEX* operator[](int idx)
	{
		if (idx < 0 || idx >= (int)blocks.size())
			return 0;

		return &blocks[idx];
	}

	__fi BASEBLOCKEX* Get(u32 startpc)
	{
		return (*this)[Index(startpc)];
	}

	__fi void Remove(int first, int last)
	{
		pxAssert(first <= last);
		int idx = first;
		do
		{
			pxAssert(idx <= last);

			//u32 startpc = blocks[idx].startpc;
			std::pair<linkiter_t, linkiter_t> range = links.equal_range(blocks[idx].startpc);
			for (linkiter_t i = range.first; i != range.second; ++i)
				*(u32*)i->second = recompiler - (i->second + 4);

			if (IsDevBuild)
			{
				// Clear the first instruction to 0xcc (breakpoint), as a way to assert if some
				// static jumps get left behind to this block.  Note: Do not clear more than the
				// first byte, since this code is called during exception handlers and event handlers
				// both of which expect to be able to return to the recompiled code.

				BASEBLOCKEX effu(blocks[idx]);
				memset((void*)effu.fnptr, 0xcc, 1);
			}
		} while (idx++ < last);

		// Erase outgoing links this block range itself created -- entries in
		// `links` whose jumpptr (the patch site) falls inside the code we're
		// discarding here. Without this, BaseBlocks::New() can later find a
		// stale entry filed under some completely unrelated target PC and
		// blindly patch through that jumpptr once this code-cache memory has
		// been handed out again to a different, currently-live block --
		// silently corrupting its machine code. Upstream TODO since 2009
		// (c483f17331); hit hard by kload's execve() into /minish, which
		// discards and immediately recompiles heavily over the same
		// code-cache addresses.
		// Look the sites up by address instead of walking every link: Full TLB discards
		// blocks thousands of times per second (needs proper testing).
		for (int j = first; j <= last; j++)
		{
			const uptr fn = blocks[j].fnptr;
			const uptr fn_end = fn + blocks[j].x86size;
			for (auto site = link_sites.lower_bound(fn); site != link_sites.end() && site->first < fn_end;)
			{
				EraseLink(site->second, site->first);
				site = link_sites.erase(site);
			}
		}

		blocks.erase(first, last + 1);
	}

	void Link(u32 pc, s32* jumpptr);

	__fi void Reset()
	{
		blocks.clear();
		links.clear();
		link_sites.clear();
	}

private:
	void EraseLink(u32 pc, uptr jumpptr)
	{
		std::pair<linkiter_t, linkiter_t> range = links.equal_range(pc);
		for (linkiter_t i = range.first; i != range.second; ++i)
		{
			if (i->second == jumpptr)
			{
				links.erase(i);
				return;
			}
		}
	}
};

#define PC_GETBLOCK_(x, reclut) ((BASEBLOCK*)(reclut[((u32)(x)) >> 16] + (x) * (sizeof(BASEBLOCK) / 4)))

/**
 * Add a page to the recompiler lookup table
 *
 * Will associate `reclut[pagebase + pageidx]` with `mapbase[mappage << 14]`
 * Will associate `hwlut[pagebase + pageidx]` with `pageidx << 16`
 */
static inline void recLUT_SetPage(uptr reclut[0x10000], u32 hwlut[0x10000],
                                  BASEBLOCK* mapbase, uint pagebase, uint pageidx, uint mappage)
{
	// this value is in 64k pages!
	uint page = pagebase + pageidx;

	pxAssert(page < 0x10000);
	reclut[page] = (uptr)&mapbase[((s32)mappage - (s32)page) << 14];
	if (hwlut)
		hwlut[page] = 0u - (pagebase << 16);
}

static_assert(sizeof(BASEBLOCK) == 8, "BASEBLOCK is not 8 bytes");
