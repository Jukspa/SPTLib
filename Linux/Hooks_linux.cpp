#include "../sptlib-stdafx.hpp"

#include "../sptlib.hpp"
#include "../MemUtils.hpp"
#include "../Hooks.hpp"
#include <dlfcn.h>
#include <link.h>

typedef void* (*_dlopen)(const char* filename, int flag);
typedef int (*_dlclose)(void* handle);
typedef void* (*_dlsym)(void* handle, const char* name);

_dlopen ORIG_dlopen;
_dlclose ORIG_dlclose;
_dlsym ORIG_dlsym;

namespace Hooks
{
	static void* get_dlsym_addr()
	{
		std::pair<void*, std::string> p = { nullptr, std::string() };
		dl_iterate_phdr([](dl_phdr_info* i, size_t s, void* data) -> int {
			if (i->dlpi_name[0])
			{
				auto name = std::string(i->dlpi_name);
				auto fileName = GetFileName(Convert(name));
				if (DebugEnabled())
					EngineDevMsg("\tName: %s\n", Convert(fileName).c_str());
				if (fileName.find(L"libdl.so") != std::wstring::npos)
				{
					auto p = reinterpret_cast<std::pair<void*, std::string>*>(data);
					p->first = reinterpret_cast<void*>(i->dlpi_addr);
					p->second.assign(i->dlpi_name);
				}
			}

			return 0;
		}, &p);

		if (!p.second.size())
			return nullptr;

		// Based on code from Matherunner's TAS Tools v2.0
	    std::FILE *libfile = std::fopen(p.second.c_str(), "r");
	    if (!libfile)
	    {
	    	perror("Error opening libdl.so");
	    	return nullptr;
	    }

	    long orig_pos = std::ftell(libfile);
	    std::fseek(libfile, 0, SEEK_END);
	    long filesize = std::ftell(libfile);
	    std::fseek(libfile, orig_pos, SEEK_SET);

	    char *filedat = new char[filesize];
	    std::fread(filedat, 1, filesize, libfile);
	    std::fclose(libfile);

	    Elf32_Ehdr *elf_hdr = (Elf32_Ehdr *)filedat;
	    Elf32_Shdr *sh_hdr = (Elf32_Shdr *)(filedat + elf_hdr->e_shoff);
	    Elf32_Shdr *sh_shstrhdr = sh_hdr + elf_hdr->e_shstrndx;
	    char *sh_shstrtab = filedat + sh_shstrhdr->sh_offset;

	    int i;
	    for (i = 0; sh_hdr[i].sh_type != SHT_DYNSYM; i++);
	    Elf32_Sym *symtab = (Elf32_Sym *)(filedat + sh_hdr[i].sh_offset);
	    uint64_t st_num_entries = sh_hdr[i].sh_size / sizeof(Elf32_Sym);

	    for (i = 0; sh_hdr[i].sh_type != SHT_STRTAB ||
	             strcmp(sh_shstrtab + sh_hdr[i].sh_name, ".dynstr") != 0; i++);
	    char *sh_strtab = filedat + sh_hdr[i].sh_offset;

		void *result = nullptr;
	    for (uint64_t i = 0; i < st_num_entries; i++)
	        if (!strcmp(sh_strtab + symtab[i].st_name, "dlsym"))
	        {
	        	result = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(p.first) + reinterpret_cast<uintptr_t>(symtab[i].st_value));
	        	break;
	        }

	    delete[] filedat;
	    return result;
	}

	extern "C" void* dlopen(const char* filename, int flag)
	{
		if (!ORIG_dlopen)
			ORIG_dlopen = reinterpret_cast<_dlopen>(dlsym(RTLD_NEXT, "dlopen"));

		auto rv = ORIG_dlopen(filename, flag);
		if (DebugEnabled())
			EngineDevMsg("Engine call: dlopen( \"%s\", %d ) => %p\n", filename, flag, rv);

		if (rv && filename)
			HookModule(Convert(filename));

		return rv;
	}

	extern "C" int dlclose(void* handle)
	{
		if (!ORIG_dlclose)
			ORIG_dlclose = reinterpret_cast<_dlclose>(dlsym(RTLD_NEXT, "dlclose"));

		for (auto it = modules.cbegin(); it != modules.cend(); ++it)
			if ((*it)->GetHandle() == handle)
				(*it)->Unhook();

		auto rv = ORIG_dlclose(handle);
		if (DebugEnabled())
			EngineDevMsg("Engine call: dlclose( %p ) => %d\n", handle, rv);

		return rv;
	}

	extern "C" void* dlsym(void* handle, const char* name)
	{
		if (!ORIG_dlsym)
			ORIG_dlsym = reinterpret_cast<_dlsym>(get_dlsym_addr());

		auto rv = ORIG_dlsym(handle, name);

		auto result = MemUtils::GetSymbolLookupResult(handle, rv);
		if (DebugEnabled())
		{
			if (result != rv)
				EngineDevMsg("Engine call: dlsym( %p, %s ) => %p [returning %p]\n", handle, name, rv, result);
			else
				EngineDevMsg("Engine call: dlsym( %p, %s ) => %p\n", handle, name, rv, result);
		}

		return result;
	}

	void InitInterception(bool needToIntercept)
	{
		if (!ORIG_dlsym)
			ORIG_dlsym = reinterpret_cast<_dlsym>(get_dlsym_addr());

		if (!ORIG_dlopen)
			ORIG_dlopen = reinterpret_cast<_dlopen>(dlsym(RTLD_NEXT, "dlopen"));

		if (!ORIG_dlclose)
			ORIG_dlclose = reinterpret_cast<_dlclose>(dlsym(RTLD_NEXT, "dlclose"));
		
		if (needToIntercept)
			MemUtils::Intercept(L"POSIX",
				ORIG_dlopen, dlopen,
				ORIG_dlclose, dlclose);
	}

	void ClearInterception(bool needToIntercept)
	{
		if (needToIntercept)
			MemUtils::RemoveInterception(L"POSIX",
				ORIG_dlopen,
				ORIG_dlclose);

		ORIG_dlopen = nullptr;
		ORIG_dlclose = nullptr;
	}
}
