/**
*  yrpp-spawner
*
*  Copyright(C) 2023-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <cstddef>
#include <cstdio>

// A streaming `.tar.zst` writer: POSIX ustar members fed through a zstd
// compression stream, so a capture's per-frame text never persists uncompressed.
//
// Why tar.zst and not a bespoke container: the analysis side already consumes
// this exact shape (`tar --zstd -xOf`, ten binaries in the ratwo repo), so a
// capture written here is readable there with no reader change, and `tar`,
// `zstd` and every language binding remain valid debugging tools.
//
// Crash resilience is the reason this is not one long zstd frame. A process
// killed mid-capture would leave a truncated frame that decoders reject
// wholesale, losing the entire run. Instead the stream is cut into complete
// zstd frames every `FramePeriod` members; zstd concatenates frames natively
// and tar tolerates a missing end-of-archive trailer, so an interrupted capture
// still reads back cleanly up to the last completed frame. Losing the tail of a
// capture is an inconvenience; losing all of it is a lost play session.
//
// Why there is no explicit row/value deduplication here, measured rather than
// assumed, because the corpus looks like it is begging for one. Over 1,000
// contiguous 08-08 frames (1.07 GB): only 2.66% of lines are distinct, and just
// 7.67% of a frame's lines differ from the previous frame's. But zstd's match
// finder IS deduplication, and it already collects nearly all of that:
//
//     this writer, level 6                6.5 MB
//     every DISTINCT line, stored once
//       and compressed  (payload floor)   2.5 MB
//     zstd level 19 alone                 3.5 MB
//
// The 2.5 MB floor is payload only -- a real dedup format must also store, per
// frame, an ordered index of which 13,588 lines that frame contains. Adding
// that lands it about where simply raising the level already is, in exchange
// for a bespoke container: no `tar --zstd`, no byte-identity with what Ares
// wrote, and a decoder to maintain on the analysis side. The constraint that
// started this was 14 GB against 13 GB free; at ~79 MB per capture it is gone,
// and 79 -> 50 buys nothing. Revisit only if a capture's SHAPE changes.
//
// Deliberately free of Windows API and of the game's types: this compiles and
// is tested on the analysis machine, leaving only the file-system glue in
// SyncDump.cpp platform-specific.
class TarZstdWriter
{
public:
	// Default members per complete zstd frame -- a ratio-vs-crash-loss dial,
	// because ending a zstd frame discards the match history that near-
	// identical consecutive capture frames depend on. Measured over 2,500
	// contiguous 08-08 frames (2.7 GB), level 3, default window:
	//
	//     period   64 -> 74.3x     period 4096 -> 92.6x
	//     period  256 -> 87.6x     never reset -> 92.6x
	//     period 1024 -> 93.5x
	//
	// 1024 is not a compromise: it BEATS never resetting, because a stale
	// long-range dictionary eventually costs more than it earns as the
	// scenario drifts. So the crash resilience is free, which is why it is on
	// by default rather than opt-in.
	static constexpr int DefaultFramePeriod = 1024;

	// log2 of the zstd match window. See the long note in Open(): the window
	// must span several ~1.1 MB capture frames or the ratio collapses, and the
	// window zstd infers from the level is the wrong lever. 0 leaves it to the
	// level. Measured defaults are in the .cpp beside the numbers.
	static constexpr int DefaultWindowLog = 24;

	// Compression level. Measured over 1,000 contiguous 08-08 frames (1.07 GB),
	// at the window above:
	//
	//     level  3 -> 156.8x  0.91 ms/frame   (~96 MB per full capture)
	//     level  6 -> 191.3x  1.59 ms/frame   (~79 MB)   <- default
	//     level  9 -> 201.6x  2.11 ms/frame   (~75 MB)
	//     level 12 -> 200.4x  2.50 ms/frame   (~75 MB)
	//
	// The knee is at 6; level 12 is both slower AND slightly worse than 9, so
	// there is nothing above this worth paying for. The cost is wall-clock
	// only -- the simulation is a lockstep logic loop, so how long a frame
	// takes to compress cannot change what the frame computes.
	static constexpr int DefaultLevel = 6;

	TarZstdWriter() = default;
	~TarZstdWriter();

	TarZstdWriter(const TarZstdWriter&) = delete;
	TarZstdWriter& operator=(const TarZstdWriter&) = delete;

	// Opens `path` for writing (truncating) at compression `level`, cutting a
	// complete zstd frame every `framePeriod` members (<= 0 means never, which
	// maximises ratio and loses everything if the process is killed). Returns
	// false and sets LastError() on failure; the object stays closed.
	bool Open(const char* path, int level = DefaultLevel,
		int framePeriod = DefaultFramePeriod, int windowLog = DefaultWindowLog);

	// Appends one member. `name` is the archive path (forward slashes).
	// Returns false and sets LastError() on any write or compression failure,
	// after which the writer closes itself rather than emit a corrupt stream.
	bool AppendBytes(const char* name, const void* data, size_t size);

	// Reads `sourcePath` whole and appends it as `name`. Returns false if the
	// source cannot be read, WITHOUT closing the writer -- a single unreadable
	// input is not a reason to abandon the archive.
	bool AppendFile(const char* name, const char* sourcePath);

	// Writes the end-of-archive trailer, ends the zstd frame and closes the
	// file. Safe to call on an already-closed writer. Returns false if the
	// trailer could not be written.
	bool Close();

	bool IsOpen() const { return File != nullptr; }
	const char* LastError() const { return Error; }

	// Members appended since Open(), for the caller's logging.
	long long MemberCount() const { return Members; }

private:
	bool WriteRaw(const void* data, size_t size, bool endFrame);
	bool EndFrame();
	void Fail(const char* message);

	FILE* File = nullptr;
	void* Stream = nullptr;      // ZSTD_CStream*, opaque to keep zstd.h out of here
	char* OutBuffer = nullptr;
	size_t OutCapacity = 0;
	long long Members = 0;
	int SinceFrameEnd = 0;
	int FramePeriod = DefaultFramePeriod;
	const char* Error = "";
};
