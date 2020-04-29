// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/image_snapshot.h"

#include "platform/assert.h"
#include "vm/compiler/backend/code_statistics.h"
#include "vm/compiler/runtime_api.h"
#include "vm/dwarf.h"
#include "vm/elf.h"
#include "vm/hash.h"
#include "vm/hash_map.h"
#include "vm/heap/heap.h"
#include "vm/instructions.h"
#include "vm/json_writer.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/program_visitor.h"
#include "vm/stub_code.h"
#include "vm/timeline.h"
#include "vm/type_testing_stubs.h"

#include <iostream>
#include <fstream>
#include <string>

namespace dart {

#if defined(DART_PRECOMPILER)
DEFINE_FLAG(bool,
            print_instruction_stats,
            false,
            "Print instruction statistics");

DEFINE_FLAG(charp,
            print_instructions_sizes_to,
            NULL,
            "Print sizes of all instruction objects to the given file");
#endif

intptr_t ObjectOffsetTrait::Hashcode(Key key) {
  RawObject* obj = key;
  ASSERT(!obj->IsSmi());

  uword body = RawObject::ToAddr(obj) + sizeof(RawObject);
  uword end = RawObject::ToAddr(obj) + obj->HeapSize();

  uint32_t hash = obj->GetClassId();
  // Don't include the header. Objects in the image are pre-marked, but objects
  // in the current isolate are not.
  for (uword cursor = body; cursor < end; cursor += sizeof(uint32_t)) {
    hash = CombineHashes(hash, *reinterpret_cast<uint32_t*>(cursor));
  }

  return FinalizeHash(hash, 30);
}

bool ObjectOffsetTrait::IsKeyEqual(Pair pair, Key key) {
  RawObject* a = pair.object;
  RawObject* b = key;
  ASSERT(!a->IsSmi());
  ASSERT(!b->IsSmi());

  if (a->GetClassId() != b->GetClassId()) {
    return false;
  }

  intptr_t heap_size = a->HeapSize();
  if (b->HeapSize() != heap_size) {
    return false;
  }

  // Don't include the header. Objects in the image are pre-marked, but objects
  // in the current isolate are not.
  uword body_a = RawObject::ToAddr(a) + sizeof(RawObject);
  uword body_b = RawObject::ToAddr(b) + sizeof(RawObject);
  uword body_size = heap_size - sizeof(RawObject);
  return 0 == memcmp(reinterpret_cast<const void*>(body_a),
                     reinterpret_cast<const void*>(body_b), body_size);
}

#if !defined(DART_PRECOMPILED_RUNTIME)
ImageWriter::ImageWriter(Heap* heap)
    : heap_(heap),
      next_data_offset_(0),
      next_text_offset_(0),
      objects_(),
      instructions_() {
  ResetOffsets();
}

void ImageWriter::PrepareForSerialization(
    GrowableArray<ImageWriterCommand>* commands) {
  if (commands != nullptr) {
    const intptr_t initial_offset = next_text_offset_;
    for (auto& inst : *commands) {
      ASSERT((initial_offset + inst.expected_offset) == next_text_offset_);
      switch (inst.op) {
        case ImageWriterCommand::InsertInstructionOfCode: {
          RawCode* code = inst.insert_instruction_of_code.code;
          RawInstructions* instructions = Code::InstructionsOf(code);
          const intptr_t offset = next_text_offset_;
          instructions_.Add(InstructionsData(instructions, code, offset));
          next_text_offset_ += SizeInSnapshot(instructions);
          ASSERT(heap_->GetObjectId(instructions) == 0);
          heap_->SetObjectId(instructions, offset);
          break;
        }
        case ImageWriterCommand::InsertBytesOfTrampoline: {
          auto trampoline_bytes = inst.insert_trampoline_bytes.buffer;
          auto trampoline_length = inst.insert_trampoline_bytes.buffer_length;
          const intptr_t offset = next_text_offset_;
          instructions_.Add(
              InstructionsData(trampoline_bytes, trampoline_length, offset));
          next_text_offset_ += trampoline_length;
          break;
        }
        default:
          UNREACHABLE();
      }
    }
  }
}

int32_t ImageWriter::GetTextOffsetFor(RawInstructions* instructions,
                                      RawCode* code) {
  intptr_t offset = heap_->GetObjectId(instructions);
  if (offset != 0) {
    return offset;
  }

  offset = next_text_offset_;
  heap_->SetObjectId(instructions, offset);
  next_text_offset_ += SizeInSnapshot(instructions);
  instructions_.Add(InstructionsData(instructions, code, offset));

  ASSERT(offset != 0);
  return offset;
}

#if defined(IS_SIMARM_X64)
static intptr_t CompressedStackMapsSizeInSnapshot(intptr_t payload_size) {
  // We do not need to round the non-payload size up to a word boundary because
  // currently sizeof(RawCompressedStackMaps) is 12, even on 64-bit.
  const intptr_t unrounded_size_in_bytes =
      compiler::target::kWordSize + sizeof(uint32_t) + payload_size;
  return Utils::RoundUp(unrounded_size_in_bytes,
                        compiler::target::ObjectAlignment::kObjectAlignment);
}

static intptr_t StringPayloadSize(intptr_t len, bool isOneByteString) {
  return len * (isOneByteString ? OneByteString::kBytesPerElement
                                : TwoByteString::kBytesPerElement);
}

static intptr_t StringSizeInSnapshot(intptr_t len, bool isOneByteString) {
  const intptr_t unrounded_size_in_bytes =
      (String::kSizeofRawString / 2) + StringPayloadSize(len, isOneByteString);
  return Utils::RoundUp(unrounded_size_in_bytes,
                        compiler::target::ObjectAlignment::kObjectAlignment);
}

static intptr_t CodeSourceMapSizeInSnapshot(intptr_t len) {
  const intptr_t unrounded_size_in_bytes =
      2 * compiler::target::kWordSize + len;
  return Utils::RoundUp(unrounded_size_in_bytes,
                        compiler::target::ObjectAlignment::kObjectAlignment);
}

static intptr_t PcDescriptorsSizeInSnapshot(intptr_t len) {
  const intptr_t unrounded_size_in_bytes =
      2 * compiler::target::kWordSize + len;
  return Utils::RoundUp(unrounded_size_in_bytes,
                        compiler::target::ObjectAlignment::kObjectAlignment);
}

static intptr_t InstructionsSizeInSnapshot(intptr_t len) {
  return Utils::RoundUp(compiler::target::Instructions::HeaderSize() + len,
                        compiler::target::ObjectAlignment::kObjectAlignment);
}

intptr_t ImageWriter::SizeInSnapshot(RawObject* raw_object) {
  const classid_t cid = raw_object->GetClassId();

  switch (cid) {
    case kCompressedStackMapsCid: {
      RawCompressedStackMaps* raw_maps =
          static_cast<RawCompressedStackMaps*>(raw_object);
      return CompressedStackMapsSizeInSnapshot(raw_maps->ptr()->payload_size());
    }
    case kOneByteStringCid:
    case kTwoByteStringCid: {
      RawString* raw_str = static_cast<RawString*>(raw_object);
      return StringSizeInSnapshot(Smi::Value(raw_str->ptr()->length_),
                                  cid == kOneByteStringCid);
    }
    case kCodeSourceMapCid: {
      RawCodeSourceMap* raw_map = static_cast<RawCodeSourceMap*>(raw_object);
      return CodeSourceMapSizeInSnapshot(raw_map->ptr()->length_);
    }
    case kPcDescriptorsCid: {
      RawPcDescriptors* raw_desc = static_cast<RawPcDescriptors*>(raw_object);
      return PcDescriptorsSizeInSnapshot(raw_desc->ptr()->length_);
    }
    case kInstructionsCid: {
      RawInstructions* raw_insns = static_cast<RawInstructions*>(raw_object);
      return InstructionsSizeInSnapshot(Instructions::Size(raw_insns));
    }
    default: {
      const Class& clazz = Class::Handle(Object::Handle(raw_object).clazz());
      FATAL1("Unsupported class %s in rodata section.\n", clazz.ToCString());
      return 0;
    }
  }
}
#else   // defined(IS_SIMARM_X64)
intptr_t ImageWriter::SizeInSnapshot(RawObject* raw_object) {
  return raw_object->HeapSize();
}
#endif  // defined(IS_SIMARM_X64)

uint32_t ImageWriter::GetDataOffsetFor(RawObject* raw_object) {
  intptr_t snap_size = SizeInSnapshot(raw_object);
  intptr_t offset = next_data_offset_;
  next_data_offset_ += snap_size;
  objects_.Add(ObjectData(raw_object));
  return offset;
}

#if defined(DART_PRECOMPILER)
void ImageWriter::DumpInstructionStats() {
  CombinedCodeStatistics instruction_stats;
  for (intptr_t i = 0; i < instructions_.length(); i++) {
    auto& data = instructions_[i];
    CodeStatistics* stats = data.insns_->stats();
    if (stats != nullptr) {
      stats->AppendTo(&instruction_stats);
    }
  }
  instruction_stats.DumpStatistics();
}

void ImageWriter::DumpInstructionsSizes() {
  auto thread = Thread::Current();
  auto zone = thread->zone();

  auto& cls = Class::Handle(zone);
  auto& lib = Library::Handle(zone);
  auto& owner = Object::Handle(zone);
  auto& url = String::Handle(zone);
  auto& name = String::Handle(zone);

  JSONWriter js;
  js.OpenArray();
  for (intptr_t i = 0; i < instructions_.length(); i++) {
    auto& data = instructions_[i];
    owner = data.code_->owner();
    js.OpenObject();
    if (owner.IsFunction()) {
      cls = Function::Cast(owner).Owner();
      name = cls.ScrubbedName();
      lib = cls.library();
      url = lib.url();
      js.PrintPropertyStr("l", url);
      js.PrintPropertyStr("c", name);
    }
    js.PrintProperty("n", data.code_->QualifiedName());
    js.PrintProperty("s", SizeInSnapshot(data.insns_->raw()));
    js.CloseObject();
  }
  js.CloseArray();

  auto file_open = Dart::file_open_callback();
  auto file_write = Dart::file_write_callback();
  auto file_close = Dart::file_close_callback();
  if ((file_open == nullptr) || (file_write == nullptr) ||
      (file_close == nullptr)) {
    return;
  }

  auto file = file_open(FLAG_print_instructions_sizes_to, /*write=*/true);
  if (file == nullptr) {
    OS::PrintErr("Failed to open file %s\n", FLAG_print_instructions_sizes_to);
    return;
  }

  char* output = nullptr;
  intptr_t output_length = 0;
  js.Steal(&output, &output_length);
  file_write(output, output_length, file);
  free(output);
  file_close(file);
}

void ImageWriter::DumpStatistics() {
  if (FLAG_print_instruction_stats) {
    DumpInstructionStats();
  }

  if (FLAG_print_instructions_sizes_to != nullptr) {
    DumpInstructionsSizes();
  }
}
#endif

void ImageWriter::Write(WriteStream* clustered_stream, bool vm) {
  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  Heap* heap = thread->isolate()->heap();
  TIMELINE_DURATION(thread, Isolate, "WriteInstructions");

  // Handlify collected raw pointers as building the names below
  // will allocate on the Dart heap.
  for (intptr_t i = 0; i < instructions_.length(); i++) {
    InstructionsData& data = instructions_[i];
    const bool is_trampoline = data.trampoline_bytes != nullptr;
    if (is_trampoline) continue;

    data.insns_ = &Instructions::Handle(zone, data.raw_insns_);
    ASSERT(data.raw_code_ != NULL);
    data.code_ = &Code::Handle(zone, data.raw_code_);

    // Reset object id as an isolate snapshot after a VM snapshot will not use
    // the VM snapshot's text image.
    heap->SetObjectId(data.insns_->raw(), 0);
  }
  for (intptr_t i = 0; i < objects_.length(); i++) {
    ObjectData& data = objects_[i];
    data.obj_ = &Object::Handle(zone, data.raw_obj_);
  }

  // Append the direct-mapped RO data objects after the clustered snapshot.
  offset_space_ = vm ? V8SnapshotProfileWriter::kVmData
                     : V8SnapshotProfileWriter::kIsolateData;
  WriteROData(clustered_stream);

  offset_space_ = vm ? V8SnapshotProfileWriter::kVmText
                     : V8SnapshotProfileWriter::kIsolateText;
  WriteText(clustered_stream, vm);
}

void ImageWriter::WriteROData(WriteStream* stream) {
  stream->Align(kMaxObjectAlignment);

  // Heap page starts here.

  intptr_t section_start = stream->Position();

  stream->WriteWord(next_data_offset_);  // Data length.
  COMPILE_ASSERT(kMaxObjectAlignment >= kObjectAlignment);
  stream->Align(kMaxObjectAlignment);

  ASSERT(stream->Position() - section_start == Image::kHeaderSize);

  // Heap page objects start here.

  for (intptr_t i = 0; i < objects_.length(); i++) {
    const Object& obj = *objects_[i].obj_;
    AutoTraceImage(obj, section_start, stream);

    NoSafepointScope no_safepoint;
    uword start = reinterpret_cast<uword>(obj.raw()) - kHeapObjectTag;
    uword end = start + obj.raw()->HeapSize();

    // Write object header with the mark and read-only bits set.
    uword marked_tags = obj.raw()->ptr()->tags_;
    marked_tags = RawObject::OldBit::update(true, marked_tags);
    marked_tags = RawObject::OldAndNotMarkedBit::update(false, marked_tags);
    marked_tags = RawObject::OldAndNotRememberedBit::update(true, marked_tags);
    marked_tags = RawObject::NewBit::update(false, marked_tags);
#if defined(HASH_IN_OBJECT_HEADER)
    marked_tags |= static_cast<uword>(obj.raw()->ptr()->hash_) << 32;
#endif

#if defined(IS_SIMARM_X64)
    if (obj.IsCompressedStackMaps()) {
      const CompressedStackMaps& map = CompressedStackMaps::Cast(obj);

      // Header layout is the same between 32-bit and 64-bit architecture, but
      // we need to recalcuate the size in words.
      const intptr_t payload_size = map.payload_size();
      const intptr_t size_in_bytes =
          CompressedStackMapsSizeInSnapshot(payload_size);
      marked_tags = RawObject::SizeTag::update(size_in_bytes * 2, marked_tags);

      stream->WriteTargetWord(marked_tags);
      // We do not need to align the stream to a word boundary on 64-bit because
      // sizeof(RawCompressedStackMaps) is 12, even there.
      stream->WriteFixed<uint32_t>(map.raw()->ptr()->flags_and_size_);
      stream->WriteBytes(map.raw()->ptr()->data(), payload_size);
      stream->Align(compiler::target::ObjectAlignment::kObjectAlignment);
    } else if (obj.IsString()) {
      const String& str = String::Cast(obj);
      RELEASE_ASSERT(String::GetCachedHash(str.raw()) != 0);
      RELEASE_ASSERT(str.IsOneByteString() || str.IsTwoByteString());
      const intptr_t size_in_bytes =
          StringSizeInSnapshot(str.Length(), str.IsOneByteString());
      marked_tags = RawObject::SizeTag::update(size_in_bytes * 2, marked_tags);

      stream->WriteTargetWord(marked_tags);
      stream->WriteTargetWord(
          reinterpret_cast<uword>(str.raw()->ptr()->length_));
      stream->WriteTargetWord(reinterpret_cast<uword>(str.raw()->ptr()->hash_));
      stream->WriteBytes(
          reinterpret_cast<const void*>(start + String::kSizeofRawString),
          StringPayloadSize(str.Length(), str.IsOneByteString()));
      stream->Align(compiler::target::ObjectAlignment::kObjectAlignment);
    } else if (obj.IsCodeSourceMap()) {
      const CodeSourceMap& map = CodeSourceMap::Cast(obj);

      const intptr_t size_in_bytes = CodeSourceMapSizeInSnapshot(map.Length());
      marked_tags = RawObject::SizeTag::update(size_in_bytes * 2, marked_tags);

      stream->WriteTargetWord(marked_tags);
      stream->WriteTargetWord(map.Length());
      stream->WriteBytes(map.Data(), map.Length());
      stream->Align(compiler::target::ObjectAlignment::kObjectAlignment);
    } else if (obj.IsPcDescriptors()) {
      const PcDescriptors& desc = PcDescriptors::Cast(obj);

      const intptr_t size_in_bytes = PcDescriptorsSizeInSnapshot(desc.Length());
      marked_tags = RawObject::SizeTag::update(size_in_bytes * 2, marked_tags);

      stream->WriteTargetWord(marked_tags);
      stream->WriteTargetWord(desc.Length());
      stream->WriteBytes(desc.raw()->ptr()->data(), desc.Length());
      stream->Align(compiler::target::ObjectAlignment::kObjectAlignment);
    } else {
      const Class& clazz = Class::Handle(obj.clazz());
      FATAL1("Unsupported class %s in rodata section.\n", clazz.ToCString());
    }
    USE(start);
    USE(end);
#else   // defined(IS_SIMARM_X64)
    stream->WriteWord(marked_tags);
    start += sizeof(uword);
    for (uword* cursor = reinterpret_cast<uword*>(start);
         cursor < reinterpret_cast<uword*>(end); cursor++) {
      stream->WriteWord(*cursor);
    }
#endif  // defined(IS_SIMARM_X64)
  }
}

AssemblyImageWriter::AssemblyImageWriter(Thread* thread,
                                         Dart_StreamingWriteCallback callback,
                                         void* callback_data)
    : ImageWriter(thread->heap()),
      assembly_stream_(512 * KB, callback, callback_data),
      dwarf_(nullptr) {
#if defined(DART_PRECOMPILER)
  Zone* zone = Thread::Current()->zone();
  dwarf_ = new (zone) Dwarf(zone, &assembly_stream_, /* elf= */ nullptr);
#endif
}

void AssemblyImageWriter::Finalize() {
#ifdef DART_PRECOMPILER
  dwarf_->Write();
#endif
}

#if !defined(DART_PRECOMPILED_RUNTIME)
static void EnsureAssemblerIdentifier(char* label) {
  for (char c = *label; c != '\0'; c = *++label) {
    if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
        ((c >= '0') && (c <= '9'))) {
      continue;
    }
    *label = '_';
  }
}

static const char* NameOfStubIsolateSpecificStub(ObjectStore* object_store,
                                                 const Code& code) {
  if (code.raw() == object_store->build_method_extractor_code()) {
    return "_iso_stub_BuildMethodExtractorStub";
  } else if (code.raw() == object_store->null_error_stub_with_fpu_regs_stub()) {
    return "_iso_stub_NullErrorSharedWithFPURegsStub";
  } else if (code.raw() ==
             object_store->null_error_stub_without_fpu_regs_stub()) {
    return "_iso_stub_NullErrorSharedWithoutFPURegsStub";
  } else if (code.raw() ==
             object_store->stack_overflow_stub_with_fpu_regs_stub()) {
    return "_iso_stub_StackOverflowStubWithFPURegsStub";
  } else if (code.raw() ==
             object_store->stack_overflow_stub_without_fpu_regs_stub()) {
    return "_iso_stub_StackOverflowStubWithoutFPURegsStub";
  } else if (code.raw() == object_store->write_barrier_wrappers_stub()) {
    return "_iso_stub_WriteBarrierWrappersStub";
  } else if (code.raw() == object_store->array_write_barrier_stub()) {
    return "_iso_stub_ArrayWriteBarrierStub";
  }
  return nullptr;
}
#endif  // !defined(DART_PRECOMPILED_RUNTIME)

const char* AssemblyCodeNamer::AssemblyNameFor(intptr_t code_index,
                                               const Code& code) {
  ASSERT(!code.IsNull());
  owner_ = code.owner();
  if (owner_.IsNull()) {
    insns_ = code.instructions();
    const char* name = StubCode::NameOfStub(insns_.EntryPoint());
    if (name != nullptr) {
      return OS::SCreate(zone_, "Precompiled_Stub_%s", name);
    } else {
      if (name == nullptr) {
        name = NameOfStubIsolateSpecificStub(store_, code);
      }
      ASSERT(name != nullptr);
      return OS::SCreate(zone_, "Precompiled__%s", name);
    }
  } else if (owner_.IsClass()) {
    string_ = Class::Cast(owner_).Name();
    const char* name = string_.ToCString();
    EnsureAssemblerIdentifier(const_cast<char*>(name));
    return OS::SCreate(zone_, "Precompiled_AllocationStub_%s_%" Pd, name,
                       code_index);
  } else if (owner_.IsAbstractType()) {
    const char* name = namer_.StubNameForType(AbstractType::Cast(owner_));
    return OS::SCreate(zone_, "Precompiled_%s", name);
  } else if (owner_.IsFunction()) {
    const char* name = Function::Cast(owner_).ToQualifiedCString();
    EnsureAssemblerIdentifier(const_cast<char*>(name));
    return OS::SCreate(zone_, "Precompiled_%s_%" Pd, name, code_index);
  } else {
    UNREACHABLE();
  }
}

void AssemblyImageWriter::WriteText(WriteStream* clustered_stream, bool vm) {
#if defined(DART_PRECOMPILED_RUNTIME)
  UNREACHABLE();
#else
  Zone* zone = Thread::Current()->zone();

#if defined(DART_PRECOMPILER)
  const char* bss_symbol =
      vm ? "_kDartVmSnapshotBss" : "_kDartIsolateSnapshotBss";
#endif

  const char* instructions_symbol =
      vm ? "_kDartVmSnapshotInstructions" : "_kDartIsolateSnapshotInstructions";
  assembly_stream_.Print(".text\n");
  assembly_stream_.Print(".globl %s\n", instructions_symbol);

  // Start snapshot at page boundary.
  ASSERT(VirtualMemory::PageSize() >= kMaxObjectAlignment);
  assembly_stream_.Print(".balign %" Pd ", 0\n", VirtualMemory::PageSize());
  assembly_stream_.Print("%s:\n", instructions_symbol);

  // This head also provides the gap to make the instructions snapshot
  // look like a HeapPage.
  intptr_t instructions_length = next_text_offset_;
  WriteWordLiteralText(instructions_length);

#if defined(DART_PRECOMPILER)
  assembly_stream_.Print("%s %s - %s\n", kLiteralPrefix, bss_symbol,
                         instructions_symbol);
#else
  WriteWordLiteralText(0);  // No relocations.
#endif

  intptr_t header_words = Image::kHeaderSize / sizeof(compiler::target::uword);
  for (intptr_t i = Image::kHeaderFields; i < header_words; i++) {
    WriteWordLiteralText(0);
  }

  FrameUnwindPrologue();

  PcDescriptors& descriptors = PcDescriptors::Handle(zone);
  AssemblyCodeNamer namer(zone);
  intptr_t text_offset = 0;

  ASSERT(offset_space_ != V8SnapshotProfileWriter::kSnapshot);
  for (intptr_t i = 0; i < instructions_.length(); i++) {
    auto& data = instructions_[i];
    const bool is_trampoline = data.trampoline_bytes != nullptr;
    ASSERT((data.text_offset_ - instructions_[0].text_offset_) == text_offset);

    if (is_trampoline) {
      if (profile_writer_ != nullptr) {
        const intptr_t offset = Image::kHeaderSize + text_offset;
        profile_writer_->SetObjectTypeAndName({offset_space_, offset},
                                              "Trampolines",
                                              /*name=*/nullptr);
        profile_writer_->AttributeBytesTo({offset_space_, offset},
                                          data.trampline_length);
      }

      const auto start = reinterpret_cast<uword>(data.trampoline_bytes);
      const auto end = start + data.trampline_length;
      text_offset += WriteByteSequence(start, end);
      delete[] data.trampoline_bytes;
      data.trampoline_bytes = nullptr;
      continue;
    }

    const intptr_t instr_start = text_offset;

    const Instructions& insns = *data.insns_;
    const Code& code = *data.code_;
    descriptors = data.code_->pc_descriptors();

    if (profile_writer_ != nullptr) {
      const intptr_t offset = Image::kHeaderSize + text_offset;
      profile_writer_->SetObjectTypeAndName({offset_space_, offset},
                                            "Instructions",
                                            /*name=*/nullptr);
      profile_writer_->AttributeBytesTo({offset_space_, offset},
                                        SizeInSnapshot(insns.raw()));
    }

    // 1. Write from the object start to the payload start. This includes the
    // object header and the fixed fields.
    {
      NoSafepointScope no_safepoint;

      // Write Instructions with the mark and read-only bits set.
      uword marked_tags = insns.raw_ptr()->tags_;
      marked_tags = RawObject::OldBit::update(true, marked_tags);
      marked_tags = RawObject::OldAndNotMarkedBit::update(false, marked_tags);
      marked_tags =
          RawObject::OldAndNotRememberedBit::update(true, marked_tags);
      marked_tags = RawObject::NewBit::update(false, marked_tags);
#if defined(HASH_IN_OBJECT_HEADER)
      // Can't use GetObjectTagsAndHash because the update methods discard the
      // high bits.
      marked_tags |= static_cast<uword>(insns.raw_ptr()->hash_) << 32;
#endif

#if defined(IS_SIMARM_X64)
      const intptr_t size_in_bytes = InstructionsSizeInSnapshot(insns.Size());
      marked_tags = RawObject::SizeTag::update(size_in_bytes * 2, marked_tags);
      WriteWordLiteralText(marked_tags);
      text_offset += sizeof(compiler::target::uword);
      WriteWordLiteralText(insns.raw_ptr()->size_and_flags_);
      text_offset += sizeof(compiler::target::uword);
      WriteWordLiteralText(insns.raw_ptr()->unchecked_entrypoint_pc_offset_);
      text_offset += sizeof(compiler::target::uword);
#else   // defined(IS_SIMARM_X64)
      uword object_start = reinterpret_cast<uword>(insns.raw_ptr());
      uword payload_start = insns.PayloadStart();
      WriteWordLiteralText(marked_tags);
      object_start += sizeof(uword);
      text_offset += sizeof(uword);
      text_offset += WriteByteSequence(object_start, payload_start);
#endif  // defined(IS_SIMARM_X64)

      ASSERT((text_offset - instr_start) ==
             compiler::target::Instructions::HeaderSize());
    }

    intptr_t dwarf_index = i;
#ifdef DART_PRECOMPILER
    // Create a label for use by DWARF.
    if ((dwarf_ != nullptr) && !code.IsNull()) {
      dwarf_index = dwarf_->AddCode(code);
    }
#endif
    // 2. Write a label at the entry point.
    // Linux's perf uses these labels.
    assembly_stream_.Print("%s:\n", namer.AssemblyNameFor(dwarf_index, code));

    {
      // 3. Write from the payload start to payload end.
      NoSafepointScope no_safepoint;
      const uword payload_start = insns.PayloadStart();
      const uword payload_size =
          Utils::RoundUp(insns.Size(), sizeof(compiler::target::uword));
      const uword payload_end = payload_start + payload_size;

#if defined(DART_PRECOMPILER)
      PcDescriptors::Iterator iterator(descriptors,
                                       RawPcDescriptors::kBSSRelocation);
      uword next_reloc_offset = iterator.MoveNext() ? iterator.PcOffset() : -1;

      for (uword cursor = payload_start; cursor < payload_end;
           cursor += sizeof(compiler::target::uword)) {
        compiler::target::uword data =
            *reinterpret_cast<compiler::target::uword*>(cursor);
        if ((cursor - payload_start) == next_reloc_offset) {
          assembly_stream_.Print("%s %s - (.) + %" Pd "\n", kLiteralPrefix,
                                 bss_symbol, /*addend=*/data);
          next_reloc_offset = iterator.MoveNext() ? iterator.PcOffset() : -1;
        } else {
          WriteWordLiteralText(data);
        }
      }
      text_offset += payload_size;
#else
      text_offset += WriteByteSequence(payload_start, payload_end);
#endif

      // 4. Write from the payload end to object end. Note we can't simply copy
      // from the object because the host object may have less alignment filler
      // than the target object in the cross-word case.
      uword unaligned_size =
          compiler::target::Instructions::HeaderSize() + payload_size;
      uword alignment_size =
          Utils::RoundUp(unaligned_size,
                         compiler::target::ObjectAlignment::kObjectAlignment) -
          unaligned_size;
      while (alignment_size > 0) {
        WriteWordLiteralText(compiler::Assembler::GetBreakInstructionFiller());
        alignment_size -= sizeof(compiler::target::uword);
        text_offset += sizeof(compiler::target::uword);
      }

      ASSERT(kWordSize != compiler::target::kWordSize ||
             (text_offset - instr_start) == insns.raw()->HeapSize());
    }

    ASSERT((text_offset - instr_start) == SizeInSnapshot(insns.raw()));
  }

  FrameUnwindEpilogue();

#if defined(DART_PRECOMPILER)
  assembly_stream_.Print(".bss\n");
  assembly_stream_.Print("%s:\n", bss_symbol);

  // Currently we only put one symbol in the data section, the address of
  // DLRT_GetThreadForNativeCallback, which is populated when the snapshot is
  // loaded.
  WriteWordLiteralText(0);
#endif

#if defined(TARGET_OS_LINUX) || defined(TARGET_OS_ANDROID) ||                  \
    defined(TARGET_OS_FUCHSIA)
  assembly_stream_.Print(".section .rodata\n");
  const char* data_symbol =
      vm ? "_kDartVmSnapshotData" : "_kDartIsolateSnapshotData";
  assembly_stream_.Print(".globl %s\n", data_symbol);
  assembly_stream_.Print(".balign %" Pd ", 0\n", kMaxObjectAlignment);
  assembly_stream_.Print("%s:\n", data_symbol);
  uword buffer = reinterpret_cast<uword>(clustered_stream->buffer());
  intptr_t length = clustered_stream->bytes_written();
  WriteByteSequence(buffer, buffer + length);
#elif defined(TARGET_OS_MACOS) || defined(TARGET_OS_MACOS_IOS)
  // assembly_stream_.Print(".const\n");
  std::string data_symbol_name = vm ? "_kDartVmSnapshotData" : "_kDartIsolateSnapshotData";
  std::ofstream ouF;
  std::string file_path_prefix = "build/aot/arm64/";
  #if defined(TARGET_ARCH_ARM) 
    file_path_prefix = "build/aot/armv7/";
  #endif
  std::string file_path = file_path_prefix + data_symbol_name;
  OS::Print("file_path  %s \n", file_path.c_str());
  ouF.open(file_path + ".dat", std::ofstream::binary);
  uword start = reinterpret_cast<uword>(clustered_stream->buffer());
  intptr_t l = clustered_stream->bytes_written();
  uword end = start + l;
  for (auto* cursor = reinterpret_cast<compiler::target::uword*>(start);
      cursor < reinterpret_cast<compiler::target::uword*>(end); cursor++) {
      ouF.write((const char *)cursor, sizeof(compiler::target::uword));
  }
  ouF.close();
#else
  UNIMPLEMENTED();
#endif


#endif  // !defined(DART_PRECOMPILED_RUNTIME)
}

void AssemblyImageWriter::FrameUnwindPrologue() {
  // Creates DWARF's .debug_frame
  // CFI = Call frame information
  // CFA = Canonical frame address
  assembly_stream_.Print(".cfi_startproc\n");

#if defined(TARGET_ARCH_X64)
  assembly_stream_.Print(".cfi_def_cfa rbp, 0\n");  // CFA is fp+0
  assembly_stream_.Print(".cfi_offset rbp, 0\n");   // saved fp is *(CFA+0)
  assembly_stream_.Print(".cfi_offset rip, 8\n");   // saved pc is *(CFA+8)
  // saved sp is CFA+16
  // Should be ".cfi_value_offset rsp, 16", but requires gcc newer than late
  // 2016 and not supported by Android's libunwind.
  // DW_CFA_expression          0x10
  // uleb128 register (rsp)        7   (DWARF register number)
  // uleb128 size of operation     2
  // DW_OP_plus_uconst          0x23
  // uleb128 addend               16
  assembly_stream_.Print(".cfi_escape 0x10, 31, 2, 0x23, 16\n");

#elif defined(TARGET_ARCH_ARM64)
  COMPILE_ASSERT(FP == R29);
  COMPILE_ASSERT(LR == R30);
  assembly_stream_.Print(".cfi_def_cfa x29, 0\n");  // CFA is fp+0
  assembly_stream_.Print(".cfi_offset x29, 0\n");   // saved fp is *(CFA+0)
  assembly_stream_.Print(".cfi_offset x30, 8\n");   // saved pc is *(CFA+8)
  // saved sp is CFA+16
  // Should be ".cfi_value_offset sp, 16", but requires gcc newer than late
  // 2016 and not supported by Android's libunwind.
  // DW_CFA_expression          0x10
  // uleb128 register (x31)       31
  // uleb128 size of operation     2
  // DW_OP_plus_uconst          0x23
  // uleb128 addend               16
  assembly_stream_.Print(".cfi_escape 0x10, 31, 2, 0x23, 16\n");

#elif defined(TARGET_ARCH_ARM)
#if defined(TARGET_OS_MACOS) || defined(TARGET_OS_MACOS_IOS)
  COMPILE_ASSERT(FP == R7);
  assembly_stream_.Print(".cfi_def_cfa r7, 0\n");  // CFA is fp+j0
  assembly_stream_.Print(".cfi_offset r7, 0\n");   // saved fp is *(CFA+0)
#else
  COMPILE_ASSERT(FP == R11);
  assembly_stream_.Print(".cfi_def_cfa r11, 0\n");  // CFA is fp+0
  assembly_stream_.Print(".cfi_offset r11, 0\n");   // saved fp is *(CFA+0)
#endif
  assembly_stream_.Print(".cfi_offset lr, 4\n");   // saved pc is *(CFA+4)
  // saved sp is CFA+8
  // Should be ".cfi_value_offset sp, 8", but requires gcc newer than late
  // 2016 and not supported by Android's libunwind.
  // DW_CFA_expression          0x10
  // uleb128 register (sp)        13
  // uleb128 size of operation     2
  // DW_OP_plus_uconst          0x23
  // uleb128 addend                8
  assembly_stream_.Print(".cfi_escape 0x10, 13, 2, 0x23, 8\n");

// libunwind on ARM may use .ARM.exidx instead of .debug_frame
#if !defined(TARGET_OS_MACOS) && !defined(TARGET_OS_MACOS_IOS)
  COMPILE_ASSERT(FP == R11);
  assembly_stream_.Print(".fnstart\n");
  assembly_stream_.Print(".save {r11, lr}\n");
  assembly_stream_.Print(".setfp r11, sp, #0\n");
#endif

#endif
}

void AssemblyImageWriter::FrameUnwindEpilogue() {
#if defined(TARGET_ARCH_ARM)
#if !defined(TARGET_OS_MACOS) && !defined(TARGET_OS_MACOS_IOS)
  assembly_stream_.Print(".fnend\n");
#endif
#endif
  assembly_stream_.Print(".cfi_endproc\n");
}

intptr_t AssemblyImageWriter::WriteByteSequence(uword start, uword end) {
  for (auto* cursor = reinterpret_cast<compiler::target::uword*>(start);
       cursor < reinterpret_cast<compiler::target::uword*>(end); cursor++) {
    WriteWordLiteralText(*cursor);
  }
  return end - start;
}

BlobImageWriter::BlobImageWriter(Thread* thread,
                                 uint8_t** instructions_blob_buffer,
                                 ReAlloc alloc,
                                 intptr_t initial_size,
                                 intptr_t bss_base,
                                 Elf* elf,
                                 Dwarf* dwarf)
    : ImageWriter(thread->heap()),
      instructions_blob_stream_(instructions_blob_buffer, alloc, initial_size),
      elf_(elf),
      dwarf_(dwarf),
      bss_base_(bss_base) {
#ifndef DART_PRECOMPILER
  RELEASE_ASSERT(elf_ == nullptr);
  RELEASE_ASSERT(dwarf_ == nullptr);
#endif
}

intptr_t BlobImageWriter::WriteByteSequence(uword start, uword end) {
  const uword size = end - start;
  instructions_blob_stream_.WriteBytes(reinterpret_cast<const void*>(start),
                                       size);
  return size;
}

void BlobImageWriter::WriteText(WriteStream* clustered_stream, bool vm) {
  const intptr_t instructions_length = next_text_offset_;
#ifdef DART_PRECOMPILER
  intptr_t segment_base = 0;
  if (elf_ != nullptr) {
    segment_base = elf_->NextMemoryOffset();
  }
#endif

  // This header provides the gap to make the instructions snapshot look like a
  // HeapPage.
  instructions_blob_stream_.WriteTargetWord(instructions_length);
#if defined(DART_PRECOMPILER)
  instructions_blob_stream_.WriteTargetWord(
      elf_ != nullptr ? bss_base_ - segment_base : 0);
#else
  instructions_blob_stream_.WriteTargetWord(0);  // No relocations.
#endif
  const intptr_t header_words =
      Image::kHeaderSize / sizeof(compiler::target::uword);
  for (intptr_t i = Image::kHeaderFields; i < header_words; i++) {
    instructions_blob_stream_.WriteTargetWord(0);
  }

  intptr_t text_offset = 0;

#if defined(DART_PRECOMPILER)
  PcDescriptors& descriptors = PcDescriptors::Handle();
  AssemblyCodeNamer namer(Thread::Current()->zone());
#endif

  NoSafepointScope no_safepoint;
  for (intptr_t i = 0; i < instructions_.length(); i++) {
    auto& data = instructions_[i];
    const bool is_trampoline = data.trampoline_bytes != nullptr;
    ASSERT((data.text_offset_ - instructions_[0].text_offset_) == text_offset);

    if (is_trampoline) {
      const auto start = reinterpret_cast<uword>(data.trampoline_bytes);
      const auto end = start + data.trampline_length;
      text_offset += WriteByteSequence(start, end);
      delete[] data.trampoline_bytes;
      data.trampoline_bytes = nullptr;
      continue;
    }

    const intptr_t instr_start = text_offset;

    const Instructions& insns = *instructions_[i].insns_;
    AutoTraceImage(insns, 0, &this->instructions_blob_stream_);

    uword object_start = reinterpret_cast<uword>(insns.raw_ptr());
    uword payload_start = insns.PayloadStart();
    uword payload_size =
        Utils::RoundUp(
            compiler::target::Instructions::HeaderSize() + insns.Size(),
            compiler::target::ObjectAlignment::kObjectAlignment) -
        compiler::target::Instructions::HeaderSize();
    uword object_end = payload_start + payload_size;

    ASSERT(Utils::IsAligned(payload_start, sizeof(uword)));

#ifdef DART_PRECOMPILER
    const Code& code = *instructions_[i].code_;
    if ((elf_ != nullptr) && (dwarf_ != nullptr) && !code.IsNull()) {
      intptr_t segment_offset = instructions_blob_stream_.bytes_written() +
                                Instructions::HeaderSize();
      dwarf_->AddCode(code, segment_base + segment_offset);
    }
#endif

    // Write Instructions with the mark and read-only bits set.
    uword marked_tags = insns.raw_ptr()->tags_;
    marked_tags = RawObject::OldBit::update(true, marked_tags);
    marked_tags = RawObject::OldAndNotMarkedBit::update(false, marked_tags);
    marked_tags = RawObject::OldAndNotRememberedBit::update(true, marked_tags);
    marked_tags = RawObject::NewBit::update(false, marked_tags);
#if defined(HASH_IN_OBJECT_HEADER)
    // Can't use GetObjectTagsAndHash because the update methods discard the
    // high bits.
    marked_tags |= static_cast<uword>(insns.raw_ptr()->hash_) << 32;
#endif

    intptr_t payload_stream_start = 0;

#if defined(IS_SIMARM_X64)
    const intptr_t start_offset = instructions_blob_stream_.bytes_written();
    const intptr_t size_in_bytes = InstructionsSizeInSnapshot(insns.Size());
    marked_tags = RawObject::SizeTag::update(size_in_bytes * 2, marked_tags);
    instructions_blob_stream_.WriteTargetWord(marked_tags);
    instructions_blob_stream_.WriteFixed<uint32_t>(
        insns.raw_ptr()->size_and_flags_);
    instructions_blob_stream_.WriteFixed<uint32_t>(
        insns.raw_ptr()->unchecked_entrypoint_pc_offset_);
    payload_stream_start = instructions_blob_stream_.Position();
    instructions_blob_stream_.WriteBytes(
        reinterpret_cast<const void*>(insns.PayloadStart()), insns.Size());
    instructions_blob_stream_.Align(
        compiler::target::ObjectAlignment::kObjectAlignment);
    const intptr_t end_offset = instructions_blob_stream_.bytes_written();
    text_offset += (end_offset - start_offset);
    USE(object_start);
    USE(object_end);
#else   // defined(IS_SIMARM_X64)
    payload_stream_start = instructions_blob_stream_.Position() +
                           (insns.PayloadStart() - object_start);

    instructions_blob_stream_.WriteWord(marked_tags);
    text_offset += sizeof(uword);
    object_start += sizeof(uword);
    text_offset += WriteByteSequence(object_start, object_end);
#endif  // defined(IS_SIMARM_X64)

#if defined(DART_PRECOMPILER)
    if (elf_ != nullptr && dwarf_ != nullptr) {
      elf_->AddStaticSymbol(elf_->NextSectionIndex(),
                            namer.AssemblyNameFor(i, code),
                            segment_base + payload_stream_start);
    }

    // Don't patch the relocation if we're not generating ELF. The regular blobs
    // format does not yet support these relocations. Use
    // Code::VerifyBSSRelocations to check whether the relocations are patched
    // or not after loading.
    if (elf_ != nullptr) {
      const intptr_t current_stream_position =
          instructions_blob_stream_.Position();

      descriptors = data.code_->pc_descriptors();

      PcDescriptors::Iterator iterator(
          descriptors, /*kind_mask=*/RawPcDescriptors::kBSSRelocation);

      while (iterator.MoveNext()) {
        const intptr_t reloc_offset = iterator.PcOffset();

        // The instruction stream at the relocation position holds an offset
        // into BSS corresponding to the symbol being resolved. This addend is
        // factored into the relocation.
        const auto addend = *reinterpret_cast<compiler::target::word*>(
            insns.PayloadStart() + reloc_offset);

        // Overwrite the relocation position in the instruction stream with the
        // (positive) offset of the start of the payload from the start of the
        // BSS segment plus the addend in the relocation.
        instructions_blob_stream_.SetPosition(payload_stream_start +
                                              reloc_offset);

        const compiler::target::word offset =
            bss_base_ - (segment_base + payload_stream_start + reloc_offset) +
            addend;
        instructions_blob_stream_.WriteTargetWord(offset);
      }

      // Restore stream position after the relocation was patched.
      instructions_blob_stream_.SetPosition(current_stream_position);
    }
#else
    USE(payload_stream_start);
#endif

    ASSERT((text_offset - instr_start) ==
           ImageWriter::SizeInSnapshot(insns.raw()));
  }

  ASSERT(instructions_blob_stream_.bytes_written() == instructions_length);

#ifdef DART_PRECOMPILER
  if (elf_ != nullptr) {
    const char* instructions_symbol = vm ? "_kDartVmSnapshotInstructions"
                                         : "_kDartIsolateSnapshotInstructions";
    intptr_t segment_base2 =
        elf_->AddText(instructions_symbol, instructions_blob_stream_.buffer(),
                      instructions_blob_stream_.bytes_written());
    ASSERT(segment_base == segment_base2);
  }
#endif
}
#endif  // !defined(DART_PRECOMPILED_RUNTIME)

ImageReader::ImageReader(const uint8_t* data_image,
                         const uint8_t* instructions_image)
    : data_image_(data_image), instructions_image_(instructions_image) {
  ASSERT(data_image != NULL);
  ASSERT(instructions_image != NULL);
}

RawApiError* ImageReader::VerifyAlignment() const {
  if (!Utils::IsAligned(data_image_, kObjectAlignment) ||
      !Utils::IsAligned(instructions_image_, kMaxObjectAlignment)) {
    return ApiError::New(
        String::Handle(String::New("Snapshot is misaligned", Heap::kOld)),
        Heap::kOld);
  }
  return ApiError::null();
}

RawInstructions* ImageReader::GetInstructionsAt(uint32_t offset) const {
  ASSERT(Utils::IsAligned(offset, kObjectAlignment));

  RawObject* result = RawObject::FromAddr(
      reinterpret_cast<uword>(instructions_image_) + offset);
  ASSERT(result->IsInstructions());
  ASSERT(result->IsMarked());

  return Instructions::RawCast(result);
}

RawObject* ImageReader::GetObjectAt(uint32_t offset) const {
  ASSERT(Utils::IsAligned(offset, kObjectAlignment));

  RawObject* result =
      RawObject::FromAddr(reinterpret_cast<uword>(data_image_) + offset);
  ASSERT(result->IsMarked());

  return result;
}

}  // namespace dart
