/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2020 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ This program is free software; you can redistribute it and/or modify         │
│ it under the terms of the GNU General Public License as published by         │
│ the Free Software Foundation; version 2 of the License.                      │
│                                                                              │
│ This program is distributed in the hope that it will be useful, but          │
│ WITHOUT ANY WARRANTY; without even the implied warranty of                   │
│ MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU             │
│ General Public License for more details.                                     │
│                                                                              │
│ You should have received a copy of the GNU General Public License            │
│ along with this program; if not, write to the Free Software                  │
│ Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA                │
│ 02110-1301 USA                                                               │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "libc/alg/arraylist.h"
#include "libc/assert.h"
#include "libc/bits/safemacros.h"
#include "libc/calls/calls.h"
#include "libc/log/check.h"
#include "libc/macros.h"
#include "libc/mem/mem.h"
#include "libc/runtime/gc.h"
#include "libc/runtime/mappings.h"
#include "libc/runtime/runtime.h"
#include "libc/sysv/consts/map.h"
#include "libc/sysv/consts/mremap.h"
#include "libc/sysv/consts/o.h"
#include "libc/sysv/consts/ok.h"
#include "libc/sysv/consts/prot.h"
#include "libc/x/x.h"
#include "tool/build/lib/elfwriter.h"
#include "tool/build/lib/interner.h"

static const Elf64_Ehdr kObjHeader = {
    .e_ident = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3, ELFCLASS64, ELFDATA2LSB, 1,
                ELFOSABI_NONE},
    .e_type = ET_REL,
    .e_machine = EM_NEXGEN32E,
    .e_version = 1,
    .e_ehsize = sizeof(Elf64_Ehdr),
    .e_shentsize = sizeof(Elf64_Shdr)};

static size_t AppendSection(struct ElfWriter *elf, const char *name,
                            int sh_type, int sh_flags) {
  ssize_t section =
      append(elf->shdrs,
             (&(Elf64_Shdr){.sh_type = sh_type,
                            .sh_flags = sh_flags,
                            .sh_entsize = elf->entsize,
                            .sh_addralign = elf->addralign,
                            .sh_offset = sh_type != SHT_NULL ? elf->wrote : 0,
                            .sh_name = intern(elf->shstrtab, name)}));
  CHECK_NE(-1, section);
  return section;
}

static size_t FinishSection(struct ElfWriter *elf) {
  size_t section = elf->shdrs->i - 1;
  elf->shdrs->p[section].sh_size =
      elf->wrote - elf->shdrs->p[section].sh_offset;
  return section;
}

static struct ElfWriterSymRef AppendSymbol(struct ElfWriter *elf,
                                           const char *name, int st_info,
                                           int st_other, size_t st_value,
                                           size_t st_size, size_t st_shndx,
                                           enum ElfWriterSymOrder slg) {
  ssize_t sym =
      append(elf->syms[slg], (&(Elf64_Sym){.st_info = st_info,
                                           .st_size = st_size,
                                           .st_value = st_value,
                                           .st_other = st_other,
                                           .st_name = intern(elf->strtab, name),
                                           .st_shndx = st_shndx}));
  CHECK_NE(-1, sym);
  return (struct ElfWriterSymRef){.slg = slg, .sym = sym};
}

static void MakeRelaSection(struct ElfWriter *elf, size_t section) {
  size_t shdr, size;
  size = (elf->relas->i - elf->relas->j) * sizeof(Elf64_Rela);
  elfwriter_align(elf, alignof(Elf64_Rela), sizeof(Elf64_Rela));
  shdr = elfwriter_startsection(
      elf,
      gc(xasprintf("%s%s", ".rela",
                   &elf->shstrtab->p[elf->shdrs->p[section].sh_name])),
      SHT_RELA, SHF_INFO_LINK);
  elf->shdrs->p[shdr].sh_info = section;
  elfwriter_reserve(elf, size);
  elfwriter_commit(elf, size);
  FinishSection(elf);
  elf->relas->j = elf->relas->i;
}

static void WriteRelaSections(struct ElfWriter *elf, size_t symtab) {
  uint32_t sym;
  size_t i, j, k;
  Elf64_Rela *rela;
  for (j = 0, i = 0; i < elf->shdrs->i; ++i) {
    if (elf->shdrs->p[i].sh_type == SHT_RELA) {
      elf->shdrs->p[i].sh_link = symtab;
      for (rela = (Elf64_Rela *)((char *)elf->map + elf->shdrs->p[i].sh_offset);
           rela <
           (Elf64_Rela *)((char *)elf->map + (elf->shdrs->p[i].sh_offset +
                                              elf->shdrs->p[i].sh_size));
           rela++, j++) {
        sym = elf->relas->p[j].symkey.sym;
        for (k = 0; k < elf->relas->p[j].symkey.slg; ++k) {
          sym += elf->syms[k]->i;
        }
        rela->r_offset = elf->relas->p[j].offset;
        rela->r_info = ELF64_R_INFO(sym, elf->relas->p[j].type);
        rela->r_addend = elf->relas->p[j].addend;
      }
    }
  }
  assert(j == elf->relas->i);
}

static size_t FlushStrtab(struct ElfWriter *elf, const char *name,
                          struct Interner *strtab) {
  size_t size = strtab->i * sizeof(strtab->p[0]);
  elfwriter_align(elf, 1, 0);
  AppendSection(elf, ".strtab", SHT_STRTAB, 0);
  mempcpy(elfwriter_reserve(elf, size), strtab->p, size);
  elfwriter_commit(elf, size);
  return FinishSection(elf);
}

static void FlushTables(struct ElfWriter *elf) {
  size_t i, size, symtab;
  elfwriter_align(elf, alignof(Elf64_Sym), sizeof(Elf64_Sym));
  symtab = AppendSection(elf, ".symtab", SHT_SYMTAB, 0);
  for (i = 0; i < ARRAYLEN(elf->syms); ++i) {
    size = elf->syms[i]->i * sizeof(Elf64_Sym);
    memcpy(elfwriter_reserve(elf, size), elf->syms[i]->p, size);
    elfwriter_commit(elf, size);
  }
  FinishSection(elf);
  elf->shdrs->p[symtab].sh_link = FlushStrtab(elf, ".strtab", elf->strtab);
  elf->ehdr->e_shstrndx = FlushStrtab(elf, ".shstrtab", elf->shstrtab);
  WriteRelaSections(elf, symtab);
  size = elf->shdrs->i * sizeof(elf->shdrs->p[0]);
  elfwriter_align(elf, alignof(elf->shdrs->p[0]), sizeof(elf->shdrs->p[0]));
  elf->ehdr->e_shoff = elf->wrote;
  elf->ehdr->e_shnum = elf->shdrs->i;
  elf->shdrs->p[symtab].sh_info =
      elf->syms[kElfWriterSymSection]->i + elf->syms[kElfWriterSymLocal]->i;
  mempcpy(elfwriter_reserve(elf, size), elf->shdrs->p, size);
  elfwriter_commit(elf, size);
}

struct ElfWriter *elfwriter_open(const char *path, int mode) {
  struct ElfWriter *elf;
  CHECK_NOTNULL((elf = calloc(1, sizeof(struct ElfWriter))));
  CHECK_NOTNULL((elf->path = strdup(path)));
  CHECK_NE(-1, asprintf(&elf->tmppath, "%s.%d", elf->path, getpid()));
  CHECK_NE(-1, (elf->fd = open(elf->tmppath,
                               O_CREAT | O_TRUNC | O_RDWR | O_EXCL, mode)));
  CHECK_NE(-1, ftruncate(elf->fd, (elf->mapsize = FRAMESIZE)));
  CHECK_NE(MAP_FAILED, (elf->map = mmap((void *)(intptr_t)kFixedMappingsStart,
                                        elf->mapsize, PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_FIXED, elf->fd, 0)));
  elf->ehdr = memcpy(elf->map, &kObjHeader, (elf->wrote = sizeof(kObjHeader)));
  elf->strtab = newinterner();
  elf->shstrtab = newinterner();
  return elf;
}

void elfwriter_close(struct ElfWriter *elf) {
  size_t i;
  FlushTables(elf);
  CHECK_NE(-1, munmap(elf->map, elf->mapsize));
  CHECK_NE(-1, ftruncate(elf->fd, elf->wrote));
  CHECK_NE(-1, close(elf->fd));
  CHECK_NE(-1, rename(elf->tmppath, elf->path));
  freeinterner(elf->shstrtab);
  freeinterner(elf->strtab);
  free(elf->shdrs->p);
  free(elf->relas->p);
  for (i = 0; i < ARRAYLEN(elf->syms); ++i) free(elf->syms[i]->p);
  free(elf);
}

void elfwriter_align(struct ElfWriter *elf, size_t addralign, size_t entsize) {
  elf->entsize = entsize;
  elf->addralign = addralign;
  elf->wrote = roundup(elf->wrote, addralign);
}

size_t elfwriter_startsection(struct ElfWriter *elf, const char *name,
                              int sh_type, int sh_flags) {
  size_t shdr = AppendSection(elf, name, sh_type, sh_flags);
  AppendSymbol(elf, "",
               sh_type != SHT_NULL ? ELF64_ST_INFO(STB_LOCAL, STT_SECTION)
                                   : ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE),
               STV_DEFAULT, 0, 0, shdr, kElfWriterSymSection);
  return shdr;
}

void *elfwriter_reserve(struct ElfWriter *elf, size_t size) {
  size_t need, greed;
  need = elf->wrote + size;
  greed = elf->mapsize;
  if (need > greed) {
    do {
      greed = greed + (greed >> 1);
    } while (need > greed);
    greed = roundup(greed, FRAMESIZE);
    CHECK_NE(-1, ftruncate(elf->fd, greed));
    CHECK_NE(MAP_FAILED, mmap((char *)elf->map + elf->mapsize,
                              greed - elf->mapsize, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, elf->fd, elf->mapsize));
    elf->mapsize = greed;
  }
  return (char *)elf->map + elf->wrote;
}

void elfwriter_commit(struct ElfWriter *elf, size_t size) {
  elf->wrote += size;
}

void elfwriter_finishsection(struct ElfWriter *elf) {
  size_t section = FinishSection(elf);
  if (elf->relas->j < elf->relas->i) MakeRelaSection(elf, section);
}

struct ElfWriterSymRef elfwriter_appendsym(struct ElfWriter *elf,
                                           const char *name, int st_info,
                                           int st_other, size_t st_value,
                                           size_t st_size) {
  return AppendSymbol(
      elf, name, st_info, st_other, st_value, st_size, elf->shdrs->i - 1,
      ELF64_ST_BIND(st_info) == STB_LOCAL ? kElfWriterSymLocal
                                          : kElfWriterSymGlobal);
}

struct ElfWriterSymRef elfwriter_linksym(struct ElfWriter *elf,
                                         const char *name, int st_info,
                                         int st_other) {
  return AppendSymbol(elf, name, st_info, st_other, 0, 0, 0,
                      kElfWriterSymGlobal);
}

void elfwriter_appendrela(struct ElfWriter *elf, uint64_t r_offset,
                          struct ElfWriterSymRef symkey, uint32_t type,
                          int64_t r_addend) {
  CHECK_NE(-1,
           append(elf->relas, (&(struct ElfWriterRela){.type = type,
                                                       .symkey = symkey,
                                                       .offset = r_offset,
                                                       .addend = r_addend})));
}