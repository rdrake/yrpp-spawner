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

#include "TarZstd.h"

// This translation unit is compiled Cdecl (Spawner.vcxproj) while the project
// default is /Gz (StdCall), so the undecorated declarations below match the
// definitions in zstd.c, which is compiled Cdecl for the same reason. Nothing
// pins the convention through a macro here: an earlier revision defined
// ZSTDLIB_VISIBLE=__cdecl and MSVC rejected it outright, because that puts the
// convention BEFORE the return type. The class's own cross-TU surface is
// pinned in the header with TARZSTD_CALL instead, where the position is legal.
//
// Relative, not <zstd.h>: this needs no AdditionalIncludeDirectories entry,
// and the project currently sets none.
#include "ThirdParty/zstd/zstd.h"

#include <cstdlib>
#include <cstring>

namespace
{
	constexpr size_t BlockSize = 512;

	// ustar header layout (POSIX.1-1988). Offsets are the reason every field
	// below is written by absolute position rather than by struct packing:
	// a struct would be at the mercy of the compiler's alignment rules for a
	// format whose whole point is a fixed byte image.
	constexpr size_t NameOffset = 0;     constexpr size_t NameSize = 100;
	constexpr size_t ModeOffset = 100;   constexpr size_t ModeSize = 8;
	constexpr size_t UidOffset = 108;    constexpr size_t UidSize = 8;
	constexpr size_t GidOffset = 116;    constexpr size_t GidSize = 8;
	constexpr size_t SizeOffset = 124;   constexpr size_t SizeFieldSize = 12;
	constexpr size_t MtimeOffset = 136;  constexpr size_t MtimeSize = 12;
	constexpr size_t ChksumOffset = 148; constexpr size_t ChksumSize = 8;
	constexpr size_t TypeOffset = 156;
	constexpr size_t MagicOffset = 257;
	constexpr size_t VersionOffset = 263;
	constexpr size_t UnameOffset = 265;
	constexpr size_t GnameOffset = 297;

	// Writes `value` as NUL-terminated octal, right-aligned and zero-padded, in
	// `width` bytes -- i.e. width-1 octal digits then '\0', which is what tar
	// expects for mode/uid/gid/size/mtime.
	void WriteOctal(char* field, size_t width, unsigned long long value)
	{
		field[width - 1] = '\0';
		for (size_t i = width - 1; i-- > 0;)
		{
			field[i] = static_cast<char>('0' + (value & 7));
			value >>= 3;
		}
	}

	// The header checksum is the unsigned sum of all 512 bytes with the
	// checksum field itself taken as eight spaces. Written as six octal digits,
	// a NUL and a space -- the historical layout every tar accepts.
	void WriteChecksum(char* header)
	{
		std::memset(header + ChksumOffset, ' ', ChksumSize);

		unsigned long sum = 0;
		for (size_t i = 0; i < BlockSize; ++i)
			sum += static_cast<unsigned char>(header[i]);

		WriteOctal(header + ChksumOffset, 7, sum);
		header[ChksumOffset + 7] = ' ';
	}
}

void TarZstdWriter::Fail(const char* message)
{
	Error = message;

	// Tear down without recursing through Close(): an already-failed stream
	// cannot be flushed, and trying would only produce a second error.
	if (Stream)
	{
		ZSTD_freeCStream(static_cast<ZSTD_CStream*>(Stream));
		Stream = nullptr;
	}
	if (File)
	{
		std::fclose(File);
		File = nullptr;
	}
	std::free(OutBuffer);
	OutBuffer = nullptr;
	OutCapacity = 0;
}

bool TARZSTD_CALL TarZstdWriter::Open(const char* path, int level, int framePeriod, int windowLog)
{
	Close();
	Error = "";
	Members = 0;
	SinceFrameEnd = 0;
	FramePeriod = framePeriod;

	OutCapacity = ZSTD_CStreamOutSize();
	OutBuffer = static_cast<char*>(std::malloc(OutCapacity));
	if (!OutBuffer)
	{
		Error = "out of memory for the zstd output buffer";
		OutCapacity = 0;
		return false;
	}

	ZSTD_CStream* stream = ZSTD_createCStream();
	if (!stream)
	{
		std::free(OutBuffer);
		OutBuffer = nullptr;
		OutCapacity = 0;
		Error = "ZSTD_createCStream failed";
		return false;
	}

	bool paramsOk = !ZSTD_isError(ZSTD_initCStream(stream, level));

	// The single most important setting here, and it is not the level. Each
	// SYNCDUMP frame is ~1.1 MB and consecutive frames are near-identical, so
	// essentially all of the compression comes from matching against PREVIOUS
	// frames -- which only happens if the match window spans more than one.
	// zstd derives windowLog from the level, and the low levels' windows are
	// smaller than one frame, which COLLAPSES the ratio rather than merely
	// reducing it. Measured over 2,500 contiguous 08-08 frames (2.7 GB):
	//
	//     level 1, level's own window ->   5.7x     (window < one frame)
	//     level 3, level's own window ->  93.5x   1.06 ms/frame
	//     level 1, windowLog 24       -> 113.7x   0.71 ms/frame
	//     level 3, windowLog 24       -> 154.0x   0.98 ms/frame  <- default
	//     level 3, windowLog 25       -> 162.8x   0.97 ms/frame
	//     level 6, windowLog 24       -> 187.4x   1.56 ms/frame
	//
	// Level 1 going 5.7x -> 113.7x on the window alone is the proof that the
	// level was never the lever. The default beats the hand-made archives this
	// replaces (119x) while costing about a millisecond per captured frame,
	// and windowLog 24 (16 MB) stays under zstd's default decoder window limit
	// of 27, so plain `tar --zstd` reads it with no special flag.
	if (paramsOk && windowLog > 0)
		paramsOk = !ZSTD_isError(
			ZSTD_CCtx_setParameter(stream, ZSTD_c_windowLog, windowLog));

	if (!paramsOk)
	{
		ZSTD_freeCStream(stream);
		std::free(OutBuffer);
		OutBuffer = nullptr;
		OutCapacity = 0;
		Error = "could not configure the zstd compressor";
		return false;
	}

	// "wb": the members are byte images and any newline translation would
	// corrupt both the 512-byte headers and the compressed payload.
	File = std::fopen(path, "wb");
	if (!File)
	{
		ZSTD_freeCStream(stream);
		std::free(OutBuffer);
		OutBuffer = nullptr;
		OutCapacity = 0;
		Error = "could not open the archive for writing";
		return false;
	}

	Stream = stream;
	return true;
}

// Pushes `size` bytes through the compressor. `endFrame` closes the current
// zstd frame so the file is a valid archive prefix at that point.
bool TarZstdWriter::WriteRaw(const void* data, size_t size, bool endFrame)
{
	if (!File || !Stream)
	{
		Error = "writer is not open";
		return false;
	}

	ZSTD_CStream* stream = static_cast<ZSTD_CStream*>(Stream);
	ZSTD_inBuffer in{ data, size, 0 };
	const ZSTD_EndDirective mode = endFrame ? ZSTD_e_end : ZSTD_e_continue;

	// ZSTD_e_continue may legitimately consume the input without producing
	// output, so the loop condition differs by mode: for a continue it is
	// "input drained", for an end it is "zstd says the frame is flushed".
	for (;;)
	{
		ZSTD_outBuffer out{ OutBuffer, OutCapacity, 0 };
		const size_t remaining = ZSTD_compressStream2(stream, &out, &in, mode);
		if (ZSTD_isError(remaining))
		{
			Fail("ZSTD_compressStream2 failed");
			return false;
		}

		if (out.pos > 0 && std::fwrite(OutBuffer, 1, out.pos, File) != out.pos)
		{
			Fail("short write to the archive (disk full?)");
			return false;
		}

		if (endFrame)
		{
			if (remaining == 0)
				break;
		}
		else if (in.pos == in.size)
		{
			break;
		}
	}

	return true;
}

bool TarZstdWriter::EndFrame()
{
	if (!WriteRaw(nullptr, 0, true))
		return false;

	// A frame that has been ended cannot take more input; re-init starts the
	// next one. The compression level is carried by the context's parameters,
	// which ZSTD_CCtx_reset(session_only) preserves.
	ZSTD_CStream* stream = static_cast<ZSTD_CStream*>(Stream);
	if (ZSTD_isError(ZSTD_CCtx_reset(stream, ZSTD_reset_session_only)))
	{
		Fail("ZSTD_CCtx_reset failed");
		return false;
	}

	SinceFrameEnd = 0;
	return true;
}

bool TARZSTD_CALL TarZstdWriter::AppendBytes(const char* name, const void* data, size_t size)
{
	if (!File || !Stream)
	{
		Error = "writer is not open";
		return false;
	}

	// ustar's name field is 100 bytes. Splitting a longer path across the
	// prefix field is possible but the capture's names are ~30 characters, so
	// refusing is honest where silently truncating would produce an archive
	// whose members do not match the frames they came from.
	const size_t nameLength = std::strlen(name);
	if (nameLength == 0 || nameLength >= NameSize)
	{
		Error = "member name is empty or too long for a ustar header";
		return false;
	}

	char header[BlockSize];
	std::memset(header, 0, sizeof(header));

	std::memcpy(header + NameOffset, name, nameLength);
	WriteOctal(header + ModeOffset, ModeSize, 0644);
	WriteOctal(header + UidOffset, UidSize, 0);
	WriteOctal(header + GidOffset, GidSize, 0);
	WriteOctal(header + SizeOffset, SizeFieldSize, size);
	// mtime 0: the capture's ordering lives in the frame-stamped member NAME,
	// and a real clock would make two runs of the same scenario differ byte for
	// byte, which is exactly the property a determinism corpus wants to keep.
	WriteOctal(header + MtimeOffset, MtimeSize, 0);
	header[TypeOffset] = '0';                          // regular file
	std::memcpy(header + MagicOffset, "ustar", 5);     // trailing NUL from memset
	std::memcpy(header + VersionOffset, "00", 2);
	std::memcpy(header + UnameOffset, "root", 4);
	std::memcpy(header + GnameOffset, "root", 4);
	WriteChecksum(header);

	if (!WriteRaw(header, sizeof(header), false))
		return false;

	if (size > 0 && !WriteRaw(data, size, false))
		return false;

	// Every member is padded to a 512-byte boundary.
	const size_t remainder = size % BlockSize;
	if (remainder != 0)
	{
		char padding[BlockSize];
		std::memset(padding, 0, sizeof(padding));
		if (!WriteRaw(padding, BlockSize - remainder, false))
			return false;
	}

	++Members;
	++SinceFrameEnd;
	if (FramePeriod > 0 && SinceFrameEnd >= FramePeriod && !EndFrame())
		return false;

	return true;
}

bool TARZSTD_CALL TarZstdWriter::AppendFile(const char* name, const char* sourcePath)
{
	if (!File || !Stream)
	{
		Error = "writer is not open";
		return false;
	}

	FILE* source = std::fopen(sourcePath, "rb");
	if (!source)
	{
		Error = "could not open the source file";
		return false;
	}

	if (std::fseek(source, 0, SEEK_END) != 0)
	{
		std::fclose(source);
		Error = "could not seek the source file";
		return false;
	}

	const long length = std::ftell(source);
	if (length < 0)
	{
		std::fclose(source);
		Error = "could not size the source file";
		return false;
	}
	std::rewind(source);

	char* buffer = nullptr;
	if (length > 0)
	{
		buffer = static_cast<char*>(std::malloc(static_cast<size_t>(length)));
		if (!buffer)
		{
			std::fclose(source);
			Error = "out of memory reading the source file";
			return false;
		}

		// A short read means the size we already wrote into the header would
		// not match the payload, which yields a silently corrupt archive --
		// so this bails rather than pad.
		if (std::fread(buffer, 1, static_cast<size_t>(length), source)
			!= static_cast<size_t>(length))
		{
			std::free(buffer);
			std::fclose(source);
			Error = "short read from the source file";
			return false;
		}
	}
	std::fclose(source);

	const bool ok = AppendBytes(name, buffer, static_cast<size_t>(length));
	std::free(buffer);
	return ok;
}

bool TARZSTD_CALL TarZstdWriter::Close()
{
	if (!File || !Stream)
	{
		// Free any half-built state from a failed Open().
		std::free(OutBuffer);
		OutBuffer = nullptr;
		OutCapacity = 0;
		return true;
	}

	// End-of-archive is two zero blocks. Written before the final frame end so
	// the archive is complete rather than merely readable.
	char trailer[BlockSize * 2];
	std::memset(trailer, 0, sizeof(trailer));
	bool ok = WriteRaw(trailer, sizeof(trailer), false);

	if (ok)
		ok = WriteRaw(nullptr, 0, true);

	if (Stream)
	{
		ZSTD_freeCStream(static_cast<ZSTD_CStream*>(Stream));
		Stream = nullptr;
	}
	if (File)
	{
		if (std::fclose(File) != 0)
			ok = false;
		File = nullptr;
	}
	std::free(OutBuffer);
	OutBuffer = nullptr;
	OutCapacity = 0;

	if (!ok && Error[0] == '\0')
		Error = "failed to finalise the archive";

	return ok;
}
