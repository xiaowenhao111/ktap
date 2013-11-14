/*
 * symbol.c
 *
 * This file is part of ktap by Jovi Zhangwei.
 *
 * Copyright (C) 2013 Azat Khuzhin <a3at.mail@gmail.com>.
 *
 * ktap is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * ktap is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "symbol.h"

#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <libelf.h>

static Elf_Scn *elf_section_by_name(Elf *elf, GElf_Ehdr *ep,
				    GElf_Shdr *shp, const char *name)
{
	Elf_Scn *scn = NULL;

	/* Elf is corrupted/truncated, avoid calling elf_strptr. */
	if (!elf_rawdata(elf_getscn(elf, ep->e_shstrndx), NULL))
		return NULL;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		char *str;

		gelf_getshdr(scn, shp);
		str = elf_strptr(elf, ep->e_shstrndx, shp->sh_name);
		if (!strcmp(name, str))
			break;
	}

	return scn;
}

/**
 * @return v_addr of "LOAD" program header, that have zero offset.
 */
static vaddr_t find_load_address(Elf *elf)
{
	GElf_Phdr header;
	size_t headers;
	vaddr_t address = 0;

	elf_getphdrnum(elf, &headers);
	while (headers-- > 0) {
		gelf_getphdr(elf, headers, &header);
		if (header.p_type != PT_LOAD || header.p_offset != 0)
			continue;

		address = header.p_vaddr;
		break;
	}

	return address;
}

static size_t elf_symbols(GElf_Shdr shdr)
{
	return shdr.sh_size / shdr.sh_entsize;
}

static size_t dso_symbols(Elf *elf, symbol_actor actor, void *arg)
{
	Elf_Data *elf_data = NULL;
	Elf_Scn *scn = NULL;
	GElf_Sym sym;
	GElf_Shdr shdr;

	size_t symbols_count = 0;
	vaddr_t load_address = find_load_address(elf);

	if (!load_address)
		return symbols_count;

	while ((scn = elf_nextscn(elf, scn))) {
		int i;

		gelf_getshdr(scn, &shdr);

		if (shdr.sh_type != SHT_SYMTAB)
			continue;

		elf_data = elf_getdata(scn, elf_data);

		for (i = 0; i < elf_symbols(shdr); i++) {
			char *name;
			vaddr_t addr;

			gelf_getsym(elf_data, i, &sym);

			if (GELF_ST_TYPE(sym.st_info) != STT_FUNC)
				continue;

			name = elf_strptr(elf, shdr.sh_link, sym.st_name);
			addr = sym.st_value - load_address;

			actor(name, addr, arg);
			++symbols_count;
		}
	}

	return symbols_count;
}

#define SDT_NOTE_TYPE 3
#define SDT_NOTE_COUNT 3
#define SDT_NOTE_SCN ".note.stapsdt"
#define SDT_NOTE_NAME "stapsdt"

static vaddr_t sdt_note_addr(Elf *elf, const char *data, size_t len, int type)
{
	vaddr_t vaddr;

	/*
	 * Three addresses need to be obtained :
	 * Marker location, address of base section and semaphore location
	 */
	union {
		Elf64_Addr a64[3];
		Elf32_Addr a32[3];
	} buf;

	/*
	 * dst and src are required for translation from file to memory
	 * representation
	 */
	Elf_Data dst = {
		.d_buf = &buf, .d_type = ELF_T_ADDR, .d_version = EV_CURRENT,
		.d_size = gelf_fsize(elf, ELF_T_ADDR, SDT_NOTE_COUNT, EV_CURRENT),
		.d_off = 0, .d_align = 0
	};

	Elf_Data src = {
		.d_buf = (void *) data, .d_type = ELF_T_ADDR,
		.d_version = EV_CURRENT, .d_size = dst.d_size, .d_off = 0,
		.d_align = 0
	};

	/* Check the type of each of the notes */
	if (type != SDT_NOTE_TYPE)
		return 0;

	if (len < dst.d_size + SDT_NOTE_COUNT)
		return 0;

	/* Translation from file representation to memory representation */
	if (gelf_xlatetom(elf, &dst, &src,
			  elf_getident(elf, NULL)[EI_DATA]) == NULL)
		return 0; /* TODO */

	memcpy(&vaddr, &buf, sizeof(vaddr));

	return vaddr;
}

static const char *sdt_note_name(Elf *elf, GElf_Nhdr *nhdr, const char *data)
{
	const char *provider = data + gelf_fsize(elf,
		ELF_T_ADDR, SDT_NOTE_COUNT, EV_CURRENT);
	const char *name = (const char *)memchr(provider, '\0',
		data + nhdr->n_descsz - provider);

	if (name++ == NULL)
		return NULL;

	return name;
}

static const char *sdt_note_data(const Elf_Data *data, size_t off)
{
	return ((data->d_buf) + off);
}

static size_t dso_sdt_notes(Elf *elf, symbol_actor actor, void *arg)
{
	GElf_Ehdr ehdr;
	Elf_Scn *scn = NULL;
	Elf_Data *data;
	GElf_Shdr shdr;
	size_t shstrndx;
	size_t next;
	GElf_Nhdr nhdr;
	size_t name_off, desc_off, offset;

	vaddr_t vaddr = 0;
	size_t symbols_count = 0;

	if (gelf_getehdr(elf, &ehdr) == NULL)
		return 0;
	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		return 0;

	/*
	 * Look for section type = SHT_NOTE, flags = no SHF_ALLOC
	 * and name = .note.stapsdt
	 */
	scn = elf_section_by_name(elf, &ehdr, &shdr, SDT_NOTE_SCN);
	if (!scn)
		return 0;
	if (!(shdr.sh_type == SHT_NOTE) || (shdr.sh_flags & SHF_ALLOC))
		return 0;

	data = elf_getdata(scn, NULL);

	for (offset = 0;
		(next = gelf_getnote(data, offset, &nhdr, &name_off, &desc_off)) > 0;
		offset = next) {
		const char *name;

		if (nhdr.n_namesz != sizeof(SDT_NOTE_NAME) ||
		    memcmp(data->d_buf + name_off, SDT_NOTE_NAME,
			    sizeof(SDT_NOTE_NAME)))
			continue;

		name = sdt_note_name(elf, &nhdr, sdt_note_data(data, desc_off));
		if (!name)
			continue;

		vaddr = sdt_note_addr(elf, sdt_note_data(data, desc_off),
					nhdr.n_descsz, nhdr.n_type);
		if (!vaddr)
			continue;

		actor(name, vaddr, arg);
		++symbols_count;
	}

	return symbols_count;
}

size_t read_dso_symbols(const char *exec, int type, symbol_actor actor, void *arg)
{
	size_t symbols_count = 0;

	Elf *elf;
	int fd;

	if (elf_version(EV_CURRENT) == EV_NONE)
		return symbols_count;

	fd = open(exec, O_RDONLY);
	if (fd < 0)
		return symbols_count;

	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (elf) {
		switch (type) {
		case FIND_SYMBOL:
			symbols_count = dso_symbols(elf, actor, arg);
			break;
		case FIND_STAPSDT_NOTE:
			symbols_count = dso_sdt_notes(elf, actor, arg);
			break;
		}

		elf_end(elf);
	}

	close(fd);
	return symbols_count;
}