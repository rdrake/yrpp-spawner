// Host-side test for TarZstdWriter. Verifies the archive against the REAL
// `tar` and `zstd`, not against a second implementation of my own reading --
// a bespoke reader would agree with a bespoke writer's shared misunderstanding.

#include "TarZstd.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* what)
{
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		++failures;
}

// One SYNCDUMP frame is ~1 MB of repetitive text; this mimics the shape so the
// ratio and the multi-frame path are exercised realistically.
static std::string FakeFrame(int frame)
{
	std::string s = "Ares synchronization log (version 21.352.1218)\n";
	char line[160];
	for (int i = 0; i < 4000; ++i)
	{
		std::snprintf(line, sizeof(line),
			"#%d:\t%08X; Abs: %d (Terrain); Type: %d (TREE%02d); "
			"Coords: %d,%d,%d; Health: %d; InLimbo: 0\n",
			i, 0x1000u + i, 4, i % 7, i % 20, frame, i, 0, 100 + i);
		s += line;
	}
	return s;
}

int main()
{
	const char* archive = "/tmp/tarzstd_out.tar.zst";
	const int kFrames = 200;   // > FramePeriod(64), so several zstd frames

	std::vector<std::string> contents;
	std::vector<std::string> names;

	{
		TarZstdWriter w;
		check(w.Open(archive, 3, 64), "Open");   // period 64 so 200 members cross several frame boundaries

		for (int f = 1; f <= kFrames; ++f)
		{
			char name[64];
			std::snprintf(name, sizeof(name), "SYNCDUMP/F%07d_SYNC0_%03d.TXT", f, f % 256);
			std::string body = FakeFrame(f);
			if (!w.AppendBytes(name, body.data(), body.size()))
			{
				std::printf("FAIL  AppendBytes(%s): %s\n", name, w.LastError());
				++failures;
				break;
			}
			names.push_back(name);
			contents.push_back(body);
		}

		check(w.MemberCount() == kFrames, "MemberCount matches appended members");
		check(w.Close(), "Close");
	}

	// 1. Real tar must list exactly the members.
	//
	//    Names only, via -tf. An earlier version read sizes out of -tvf BY
	//    FIELD POSITION, which is not portable: BSD tar prints owner and group
	//    as two fields where GNU tar prints one "owner/group", so the size sits
	//    in a different column and the check would have compared the wrong
	//    token. Sizes are covered far better by the byte-identity pass below.
	{
		FILE* p = popen("tar --use-compress-program=unzstd -tf /tmp/tarzstd_out.tar.zst 2>&1", "r");
		int listed = 0;
		char line[512];
		while (p && std::fgets(line, sizeof(line), p))
			if (std::strstr(line, "SYNCDUMP/F"))
				++listed;
		if (p) pclose(p);
		std::printf("     tar listed %d members\n", listed);
		check(listed == kFrames, "tar lists every member");
	}

	// 2. EVERY member's extracted bytes must be identical -- the check that
	//    catches a wrong header size, a missing pad, or a mangled payload.
	//    One extraction, then compare on disk: doing it per member through
	//    popen was slow enough that it only ever ran on a handful, which is
	//    how a fragile size check came to carry the rest of the weight.
	{
		if (std::system("rm -rf /tmp/tarzstd_x && mkdir -p /tmp/tarzstd_x && "
			"tar --use-compress-program=unzstd -xf /tmp/tarzstd_out.tar.zst "
			"-C /tmp/tarzstd_x 2>/dev/null") != 0)
		{
			check(false, "extract the archive for comparison");
		}
		else
		{
			int matched = 0, differed = 0;
			for (size_t i = 0; i < names.size(); ++i)
			{
				const std::string path = "/tmp/tarzstd_x/" + names[i];
				std::string got;
				if (FILE* f = std::fopen(path.c_str(), "rb"))
				{
					char buf[65536];
					size_t n;
					while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
						got.append(buf, n);
					std::fclose(f);
				}
				if (got == contents[i])
					++matched;
				else if (++differed <= 3)
					std::printf("     %s differs: got %zu bytes, want %zu\n",
						names[i].c_str(), got.size(), contents[i].size());
			}
			std::printf("     %d of %zu members byte-identical\n", matched, names.size());
			check(matched == static_cast<int>(names.size()) && differed == 0,
				"every member survives the round trip byte-for-byte");
		}
		std::system("rm -rf /tmp/tarzstd_x");
	}

	// 3. The whole point: the archive must be far smaller than the input.
	{
		unsigned long long raw = 0;
		for (const auto& c : contents) raw += c.size();
		FILE* f = std::fopen(archive, "rb");
		std::fseek(f, 0, SEEK_END);
		const long long comp = std::ftell(f);
		std::fclose(f);
		std::printf("     raw %llu bytes -> archive %lld bytes (%.1fx)\n",
			raw, comp, static_cast<double>(raw) / static_cast<double>(comp));
		check(comp > 0 && static_cast<unsigned long long>(comp) < raw / 20,
			"archive is at least 20x smaller than the raw frames");
	}

	// 4. CRASH RESILIENCE, the reason for FramePeriod. Truncating the archive
	//    mid-stream must still yield whole members, not nothing. A writer using
	//    one long zstd frame fails this.
	{
		FILE* in = std::fopen(archive, "rb");
		std::fseek(in, 0, SEEK_END);
		const long long total = std::ftell(in);
		std::fseek(in, 0, SEEK_SET);
		const long long keep = total / 2;
		std::vector<char> buf(static_cast<size_t>(keep));
		size_t got = std::fread(buf.data(), 1, buf.size(), in);
		std::fclose(in);

		FILE* out = std::fopen("/tmp/tarzstd_trunc.tar.zst", "wb");
		std::fwrite(buf.data(), 1, got, out);
		std::fclose(out);

		FILE* p = popen("tar --use-compress-program=unzstd -tf /tmp/tarzstd_trunc.tar.zst 2>/dev/null | wc -l", "r");
		int recovered = 0;
		if (p) { std::fscanf(p, "%d", &recovered); pclose(p); }
		std::printf("     truncated to 50%%: %d members still readable\n", recovered);
		check(recovered > 0, "a truncated archive still yields whole members");
	}

	std::printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
	return failures == 0 ? 0 : 1;
}
