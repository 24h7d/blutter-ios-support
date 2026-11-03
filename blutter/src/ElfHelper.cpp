#include "pch.h"
#include "ElfHelper.h"
PRAGMA_WARNING(push, 0)
#include <platform/elf.h>
#if defined(DART_TARGET_OS_MACOS)
#include <platform/mach_o.h>
#endif
PRAGMA_WARNING(pop)
#include <algorithm>
#include <stdexcept>
#include <vector>
#if defined(_WIN32) || defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
//#include <dlfcn.h>
#include <sys/mman.h>
#endif // #if defined(_WIN32) || defined(WIN32)

struct ElfIdent {
	uint8_t ei_magic[4];
	uint8_t ei_class;
	uint8_t ei_data;
	uint8_t ei_version;
	uint8_t ei_osabi;
	uint8_t ei_abiversion;
	uint8_t pad1[7];
};

using namespace dart::elf;

#ifdef _WIN32
static void* load_map_file(const char* path)
{
	HANDLE hFile = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		printf("\nCannot find %s\n", path);
		return NULL;
	}

	// because Dart API requires only snapshot buffer addresses (no relative access across snapshot),
	//   so we can just mapping a whole file and find address of snapshots
	HANDLE hMapFile = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hMapFile == INVALID_HANDLE_VALUE)
		return NULL;

	// need RW because dart initialization need writing data in BSS
	void* mem = MapViewOfFile(hMapFile, FILE_MAP_COPY, 0, 0, 0);
	CloseHandle(hMapFile);

	CloseHandle(hFile);
	return mem;
}
#else
static void* load_map_file(const char* path)
{
	// need RW because dart initialization need writing data in BSS
	int fd = open(path, O_RDONLY);
	struct stat st;

	fstat(fd, &st);
	void* mem = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);

	close(fd);
	return mem;
}
#endif

LibAppInfo ElfHelper::findSnapshots(const uint8_t* elf)
{
	const auto* hdr = (const ElfHeader*)elf;
	if (hdr->section_table_entry_size != sizeof(SectionHeader))
		throw std::invalid_argument("ELF: Invalid section entry size");

	const auto* section = (SectionHeader*)(elf + hdr->section_table_offset);
	const auto sh_num = hdr->num_section_headers;

	// find .dynstr and .dynsym sections, so we can map the section names
	const char* dynstr = nullptr;
	const Symbol* dynsym = nullptr;
	const Symbol* dynsym_end = nullptr;
	for (uint16_t i = 0; i < sh_num; i++, section++) {
		if (section->type == SectionHeaderType::SHT_STRTAB && dynstr == nullptr) {
			// we want only .dynstr for .dynsym
			const char* strtab = (const char*)elf + section->file_offset;
			const char* last = strtab + section->file_size;
			const char* s_first = kVmSnapshotDataAsmSymbol;
			const char* s_last = s_first + strlen(kVmSnapshotDataAsmSymbol) + 1;
			//if (memmem(strtab, section->s_size, kVmSnapshotDataAsmSymbol, strlen(kVmSnapshotDataAsmSymbol))) {
			if (std::search(strtab, last, s_first, s_last) != last) {
				// found it
				dynstr = strtab;
			}
		}
		if (section->type == SectionHeaderType::SHT_DYNSYM) {
			if (section->entry_size != sizeof(Symbol))
				throw std::invalid_argument("ELF: Invalid DYNSYM entry size");
			dynsym = (Symbol*)(elf + section->file_offset);
			dynsym_end = (Symbol*)(elf + section->file_offset + section->file_size);
		}
		if (dynsym != nullptr && dynstr != nullptr)
			break;
	}

	// find the required symbol addresses
	const uint8_t* vm_snapshot_data = nullptr;
	const uint8_t* vm_snapshot_instructions = nullptr;
	const uint8_t* isolate_snapshot_data = nullptr;
	const uint8_t* isolate_snapshot_instructions = nullptr;
	for (; dynsym < dynsym_end; dynsym++) {
		if (dynsym->info == 0)
			continue;

		const char* name = dynstr + dynsym->name;
		// Note: sym_size is no needed for dart VM (its blob contains size)
		if (strcmp(name, kVmSnapshotDataAsmSymbol) == 0) {
			vm_snapshot_data = elf + dynsym->value;
		}
		else if (strcmp(name, kVmSnapshotInstructionsAsmSymbol) == 0) {
			vm_snapshot_instructions = elf + dynsym->value;
		}
		else if (strcmp(name, kIsolateSnapshotDataAsmSymbol) == 0) {
			isolate_snapshot_data = elf + dynsym->value;
		}
		else if (strcmp(name, kIsolateSnapshotInstructionsAsmSymbol) == 0) {
			isolate_snapshot_instructions = elf + dynsym->value;
		}
	}

	if (vm_snapshot_data == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Data");
	if (vm_snapshot_instructions == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Instructions");
	if (isolate_snapshot_data == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Data");
	if (isolate_snapshot_instructions == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Instructions");

	return LibAppInfo{
		.lib = elf,
		.vm_snapshot_data = vm_snapshot_data,
		.vm_snapshot_instructions = vm_snapshot_instructions,
		.isolate_snapshot_data = isolate_snapshot_data,
		.isolate_snapshot_instructions = isolate_snapshot_instructions,
	};
}

LibAppInfo ElfHelper::MapLibAppSo(const char* path)
{
	void* lib = load_map_file(path);
	// quick and dirty parsing ELF to get symbol addresses
	uint8_t* elf = (uint8_t*)(lib);
#if defined(DART_TARGET_OS_MACOS)
	// quick and dirty parsing of Mach-O to get Dart snapshot symbol addresses.
	// The file is mmap'd flat (load_map_file), so like the ELF path above we
	// translate a symbol's vmaddr to a FILE OFFSET (via the segment that
	// contains it) and index into `elf` with that offset directly.

	// iOS app binaries are commonly shipped as a FAT/universal container even
	// when they hold only a single arm64 slice. Unwrap it first so `elf` below
	// points at the start of the actual thin Mach-O slice.
	{
		uint32_t magic0 = *(const uint32_t*)elf;
		constexpr uint32_t FAT_MAGIC = 0xcafebabeu;
		constexpr uint32_t FAT_CIGAM = 0xbebafecau; // FAT_MAGIC as read on a little-endian host
		if (magic0 == FAT_MAGIC || magic0 == FAT_CIGAM) {
			struct FatHeader { uint32_t magic, nfat_arch; };
			struct FatArch { uint32_t cputype, cpusubtype, offset, size, align; };
			auto* fh = (const FatHeader*)elf;
			uint32_t nfat = __builtin_bswap32(fh->nfat_arch);
			auto* arch = (const FatArch*)(elf + sizeof(FatHeader));
			uint64_t slice_offset = 0;
			bool found = false;
			for (uint32_t i = 0; i < nfat; i++, arch++) {
				uint32_t cputype = __builtin_bswap32(arch->cputype);
				if (cputype == (uint32_t)dart::mach_o::CPU_TYPE_ARM64) {
					slice_offset = __builtin_bswap32(arch->offset);
					found = true;
					break;
				}
			}
			if (!found)
				throw std::invalid_argument("Mach-O: No arm64 slice found in FAT binary");
			elf = elf + slice_offset;
		}
	}

	auto* header = (dart::mach_o::mach_header_64*)elf;
	if (header->magic == dart::mach_o::MH_MAGIC || header->magic == dart::mach_o::MH_CIGAM)
		throw std::invalid_argument("Mach-O: Support only 64 bits");
	if (header->magic == dart::mach_o::MH_CIGAM_64)
		throw std::invalid_argument("Mach-O: Expected a host endian header");
	if (header->magic != dart::mach_o::MH_MAGIC_64)
		throw std::invalid_argument("Mach-O: Invalid magic header");

	struct SegRange { uint64_t vmaddr, vmsize, fileoff; };
	std::vector<SegRange> segs;
	const dart::mach_o::symtab_command* symtab = nullptr;
	const dart::mach_o::dysymtab_command* dysymtab = nullptr;

	{
		const uint8_t* cmd = elf + sizeof(dart::mach_o::mach_header_64);
		for (uint32_t i = 0; i < header->ncmds; i++) {
			auto* lc = (const dart::mach_o::load_command*)cmd;
			if (lc->cmd == dart::mach_o::LC_SEGMENT_64) {
				auto* seg = (const dart::mach_o::segment_command_64*)cmd;
				segs.push_back({ seg->vmaddr, seg->vmsize, seg->fileoff });
			} else if (lc->cmd == dart::mach_o::LC_SYMTAB) {
				symtab = (const dart::mach_o::symtab_command*)cmd;
			} else if (lc->cmd == dart::mach_o::LC_DYSYMTAB) {
				dysymtab = (const dart::mach_o::dysymtab_command*)cmd;
			}
			cmd += lc->cmdsize;
		}
	}
	if (symtab == nullptr)
		throw std::invalid_argument("Mach-O: Could not locate symbol table.");
	if (dysymtab == nullptr)
		throw std::invalid_argument("Mach-O: Could not locate dynamic symbol table.");

	auto vmaddrToFileOffset = [&](uint64_t vmaddr) -> uint64_t {
		for (auto& s : segs) {
			if (vmaddr >= s.vmaddr && vmaddr < s.vmaddr + s.vmsize)
				return s.fileoff + (vmaddr - s.vmaddr);
		}
		throw std::invalid_argument("Mach-O: symbol vmaddr not within any segment");
	};

	const char* strtab = (const char*)(elf + symtab->stroff);
	auto* nl = (const dart::mach_o::nlist*)(elf + symtab->symoff) + dysymtab->iextdefsym;

	const uint8_t* vm_snapshot_data = nullptr;
	const uint8_t* vm_snapshot_instructions = nullptr;
	const uint8_t* isolate_snapshot_data = nullptr;
	const uint8_t* isolate_snapshot_instructions = nullptr;

	for (uint32_t i = 0; i < dysymtab->nextdefsym; i++) {
		const char* name = strtab + nl[i].n_idx;
		// Note: kVmSnapshotDataAsmSymbol etc. already include the leading '_'
		// that Mach-O uses for C symbol names, so compare as-is.
		const uint8_t** output = nullptr;
		if (strcmp(name, kVmSnapshotDataAsmSymbol) == 0) output = &vm_snapshot_data;
		else if (strcmp(name, kVmSnapshotInstructionsAsmSymbol) == 0) output = &vm_snapshot_instructions;
		else if (strcmp(name, kIsolateSnapshotDataAsmSymbol) == 0) output = &isolate_snapshot_data;
		else if (strcmp(name, kIsolateSnapshotInstructionsAsmSymbol) == 0) output = &isolate_snapshot_instructions;
		if (output != nullptr)
			*output = elf + vmaddrToFileOffset(nl[i].n_value);
	}

	if (vm_snapshot_data == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart VM Snapshot Data");
	if (vm_snapshot_instructions == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart VM Snapshot Instructions");
	if (isolate_snapshot_data == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart Isolate Snapshot Data");
	if (isolate_snapshot_instructions == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart Isolate Snapshot Instructions");

	return LibAppInfo{
		.lib = elf,
		.vm_snapshot_data = vm_snapshot_data,
		.vm_snapshot_instructions = vm_snapshot_instructions,
		.isolate_snapshot_data = isolate_snapshot_data,
		.isolate_snapshot_instructions = isolate_snapshot_instructions,
	};
#else
	const auto* hdr = (ElfHeader*)elf;
	const auto* ident = (ElfIdent*)hdr->ident;
	if (memcmp(ident->ei_magic, "\x7f" "ELF", 4) != 0)
		throw std::invalid_argument("ELF: Invalid magic header"); // need ELF file
	if (ident->ei_data != 1)
		throw std::invalid_argument("ELF: Support only little endian"); // expect little-endian

	if (ident->ei_class != ELFCLASS64) { // 1 is 32 bits, 2 is 64 bits
		throw std::invalid_argument("ELF: Support only 64 bits"); // support only 64 bits
	}
	// expected e_machine
	//   3: x86, 0x28: ARM
	//   0x3e: x86-64, 0xB7: Aarch64
	// EM_386, EM_ARM, EM_X86_64, EM_AARCH64
	//hdr->e_machine;
#endif

	return findSnapshots(elf);
}
