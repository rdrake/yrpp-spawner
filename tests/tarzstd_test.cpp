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

	// 1. Real tar must list exactly the members, with exactly the sizes.
	{
		FILE* p = popen("tar --use-compress-program=unzstd -tvf /tmp/tarzstd_out.tar.zst 2>&1", "r");
		int listed = 0;
		bool sizesOk = true;
		char line[512];
		while (p && std::fgets(line, sizeof(line), p))
		{
			if (std::strstr(line, "SYNCDUMP/F"))
			{
				// BSD tar: -rw-r--r--  0 root root <size> <date> <name>
				unsigned long long size = 0;
				const char* n = std::strstr(line, "SYNCDUMP/F");
				if (std::sscanf(line, "%*s %*s %*s %*s %llu", &size) == 1)
				{
					std::string nm(n);
					while (!nm.empty() && (nm.back() == '\n' || nm.back() == '\r'))
						nm.pop_back();
					for (size_t i = 0; i < names.size(); ++i)
						if (names[i] == nm && contents[i].size() != size)
							sizesOk = false;
				}
				++listed;
			}
		}
		if (p) pclose(p);
		std::printf("     tar listed %d members\n", listed);
		check(listed == kFrames, "tar lists every member");
		check(sizesOk, "tar reports the exact byte size of every member");
	}

	// 2. Extracted bytes must be identical -- the check that catches a wrong
	//    header size, a missing pad, or a mangled compressed payload.
	{
		bool allMatch = true;
		for (int i : {0, 1, 63, 64, 65, kFrames - 1})
		{
			char cmd[256];
			std::snprintf(cmd, sizeof(cmd),
				"tar --use-compress-program=unzstd -xOf /tmp/tarzstd_out.tar.zst '%s' 2>/dev/null",
				names[i].c_str());
			FILE* p = popen(cmd, "r");
			std::string got;
			char buf[65536];
			size_t n;
			while (p && (n = std::fread(buf, 1, sizeof(buf), p)) > 0)
				got.append(buf, n);
			if (p) pclose(p);
			if (got != contents[i])
			{
				std::printf("     member %d differs: got %zu bytes, want %zu\n",
					i, got.size(), contents[i].size());
				allMatch = false;
			}
		}
		check(allMatch, "extracted bytes are byte-identical (incl. across frame boundaries 63/64/65)");
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
