#include <fstream>
#include <iostream>
#include <mutex>

#include "BKE_collection.h"
#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_object.h"
#include "BLI_set.h"
#include "BLI_string.h"
#include "BLI_string_ref.h"
#include "BLI_vector.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_space_types.h"
#include "RNA_access.h"
#include "WM_api.h"
#include "WM_types.h"
#include "object_intern.h"

struct float3 {
  float x, y, z;
};

struct float2 {
  float x, y;
};

using BLI::Set;
using BLI::StringRef;
using BLI::Vector;

class TextLinesReader {
 private:
  std::istream &m_istream;
  std::mutex m_mutex;
  Set<const char *> m_chunks;

 public:
  TextLinesReader(std::istream &istream) : m_istream(istream)
  {
  }

  ~TextLinesReader()
  {
    for (const char *ptr : m_chunks) {
      MEM_freeN((void *)ptr);
    }
  }

  bool eof() const
  {
    return m_istream.eof();
  }

  /* The returned string does not necessarily contain the final newline. */
  StringRef read_next_line_chunk(uint approximate_size)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    StringRef chunk = this->read_next_line_chunk_internal(approximate_size);
    m_chunks.add_new(chunk.data());
    return chunk;
  }

  void free_chunk(StringRef chunk)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char *ptr = chunk.data();
    m_chunks.remove(ptr);
    MEM_freeN((void *)ptr);
  }

 private:
  StringRef read_next_line_chunk_internal(uint approximate_size)
  {
    approximate_size = std::max<uint>(1, approximate_size);
    std::streampos start_pos = m_istream.tellg();
    char *buffer = (char *)MEM_mallocN(approximate_size, __func__);
    m_istream.read(buffer, approximate_size);
    int extracted_amount = m_istream.gcount();

    /* Buffer goes to end of file. So return the entire remaining buffer. */
    if (m_istream.eof()) {
      return StringRef(buffer, extracted_amount);
    }

    /* Search the last line ending. */
    /* TODO: multi-line handling */
    char *buffer_end = buffer + extracted_amount;
    while (buffer_end > buffer && *buffer_end != '\n') {
      buffer_end--;
    }

    /* The buffer contains part of a single line. Try again with a larger buffer. */
    if (buffer == buffer_end) {
      MEM_freeN(buffer);
      m_istream.seekg(start_pos);
      return this->read_next_line_chunk_internal(approximate_size * 2);
    }

    int chunk_size = buffer_end - buffer;
    /* Seek to start of a new line. */
    m_istream.seekg(chunk_size - extracted_amount + 1, std::ios::cur);

    return StringRef(buffer, chunk_size);
  }
};

enum class ObjFileSegmentType {
  mtllib,
  o,
  v,
  vt,
  vn,
  usemtl,
  s,
  f,
};

struct ObjFileSegment {
  ObjFileSegmentType type;

  ObjFileSegment(ObjFileSegmentType type) : type(type)
  {
  }

  virtual ~ObjFileSegment()
  {
  }
};

struct ObjFileSegment_mtllib : public ObjFileSegment {
  Vector<std::string> file_names;

  ObjFileSegment_mtllib() : ObjFileSegment(ObjFileSegmentType::mtllib)
  {
  }
};

struct ObjFileSegment_o : public ObjFileSegment {
  std::string name;

  ObjFileSegment_o(StringRef name) : ObjFileSegment(ObjFileSegmentType::o), name(name)
  {
  }
};

struct ObjFileSegment_v : public ObjFileSegment {
  Vector<float3> positions;

  ObjFileSegment_v() : ObjFileSegment(ObjFileSegmentType::v)
  {
  }
};

struct ObjFileSegment_vt : public ObjFileSegment {
  Vector<float2> uvs;

  ObjFileSegment_vt() : ObjFileSegment(ObjFileSegmentType::vt)
  {
  }
};

struct ObjFileSegment_vn : public ObjFileSegment {
  Vector<float3> normals;

  ObjFileSegment_vn() : ObjFileSegment(ObjFileSegmentType::vn)
  {
  }
};

struct ObjFileSegment_usemtl : public ObjFileSegment {
  std::string material_name;

  ObjFileSegment_usemtl(StringRef material_name)
      : ObjFileSegment(ObjFileSegmentType::usemtl), material_name(material_name)
  {
  }
};

struct ObjFileSegment_s : public ObjFileSegment {
  std::string smoothing_group;

  ObjFileSegment_s(StringRef smoothing_group)
      : ObjFileSegment(ObjFileSegmentType::s), smoothing_group(smoothing_group)
  {
  }
};

struct ObjFileSegment_f : public ObjFileSegment {
  Vector<uint> face_offsets;
  Vector<uint> vertex_counts;

  Vector<uint> position_indices;
  Vector<uint> uv_indices;
  Vector<uint> normal_indices;

  ObjFileSegment_f() : ObjFileSegment(ObjFileSegmentType::f)
  {
  }
};

struct ObjFileSegments {
  Vector<std::unique_ptr<ObjFileSegment>> segments;
};

template<typename FuncT> static uint count_while(StringRef str, const FuncT &func)
{
  uint count = 0;
  for (uint c : str) {
    if (func(c)) {
      count++;
    }
    else {
      break;
    }
  }
  return count;
}

static bool is_whitespace(char c)
{
  return ELEM(c, ' ', '\t', '\r');
}

static bool is_not_newline(char c)
{
  return c != '\n';
}

static bool is_newline(char c)
{
  return c == '\n';
}

static std::pair<uint, uint> find_next_word_in_line(StringRef str)
{
  uint offset = 0;
  for (char c : str) {
    if (!is_whitespace(c)) {
      break;
    }
    offset++;
  }

  uint length = 0;
  for (char c : str.drop_prefix(offset)) {
    if (is_whitespace(c) || c == '\n') {
      break;
    }
    length++;
  }

  return {offset, length};
}

class StringRefStream {
 private:
  const char *m_current;
  const char *m_end;

 public:
  StringRefStream(StringRef str) : m_current(str.begin()), m_end(str.end())
  {
  }

  bool has_remaining_chars() const
  {
    return m_current < m_end;
  }

  char peek_next() const
  {
    BLI_assert(this->has_remaining_chars());
    return m_current[0];
  }

  StringRef remaining_str() const
  {
    return StringRef(m_current, m_end - m_current);
  }

  bool startswith(StringRef other) const
  {
    return this->remaining_str().startswith(other);
  }

  bool startswith_lower_ascii(StringRef other) const
  {
    return this->remaining_str().startswith_lower_ascii(other);
  }

  bool startswith_and_forward_over(StringRef other)
  {
    if (this->startswith(other)) {
      m_current += other.size();
      return true;
    }
    else {
      return false;
    }
  }

  /* Might not end with a newline character. */
  StringRef extract_line()
  {
    const char *start = m_current;
    while (m_current < m_end && *m_current != '\n') {
      m_current++;
    }
    if (m_current < m_end) {
      m_current++;
    }
    return StringRef(start, m_current - start);
  }

  StringRef extract_until(char c)
  {
    const char *start = m_current;
    while (m_current < m_end && *m_current != c) {
      m_current++;
    }
    return StringRef(start, m_current - start);
  }

  StringRef extract_quoted_string(char quote)
  {
    BLI_assert(this->peek_next() == quote);
    m_current++;
    StringRef str = this->extract_until(quote);
    if (m_current < m_end) {
      m_current++;
    }
    return str;
  }

  void forward_over_whitespace()
  {
    while (m_current < m_end && is_whitespace(*m_current)) {
      m_current++;
    }
  }

  void forward(uint i)
  {
    m_current += i;
    BLI_assert(m_current <= m_end);
  }

  StringRef extract_including_ext(StringRef ext)
  {
    const char *start = m_current;
    while (m_current < m_end) {
      if (this->startswith_lower_ascii(ext)) {
        m_current += ext.size();
        if (m_current == m_end || ELEM(*m_current, ' ', '\t', '\r', '\n')) {
          return StringRef(start, m_current - start);
        }
      }
      else {
        m_current++;
      }
    }
    return "";
  }
};

static void parse_file_names(StringRef str, StringRef ext, Vector<std::string> &r_names)
{
  if (str.endswith('\n')) {
    str = str.drop_suffix("\n");
  }
  StringRefStream stream(str);
  while (true) {
    stream.forward_over_whitespace();
    if (!stream.has_remaining_chars()) {
      return;
    }
    if (stream.peek_next() == '"') {
      StringRef name = stream.extract_quoted_string('"');
      r_names.append(name);
    }
    else {
      StringRef name = stream.extract_including_ext(ext);
      r_names.append(name);
    }
  }
}

static std::unique_ptr<ObjFileSegments> parse_obj_lines(StringRef orig_str)
{
  StringRefStream stream(orig_str);

  auto segments = BLI::make_unique<ObjFileSegments>();

  while (stream.has_remaining_chars()) {
    StringRef line = stream.extract_line();
    if (line.size() == 0) {
      continue;
    }
    switch (line[0]) {
      case ' ':
      case '\t':
      case '\r': {
        break;
      }
      case '#': {
        break;
      }
      case 'm': {
        if (line.startswith("mtllib")) {
          auto segment = BLI::make_unique<ObjFileSegment_mtllib>();
          parse_file_names(line.drop_prefix("mtllib"), ".mtl", segment->file_names);
          segment->file_names.as_ref().print_as_lines("File Names");
        }
        break;
      }
      case 'o': {
        // StringRef str = orig_str.drop_prefix(offset + strlen("o"));
        // std::pair<uint, uint> word_span = find_next_word_in_line(str);
        // StringRef object_name = str.substr(word_span.first, word_span.second);
        // auto segment = BLI::make_unique<ObjFileSegment_o>(object_name);
        // segments->segments.append(std::move(segment));
        // offset += strlen("0") + word_span.first + word_span.second;

        // offset += count_while(orig_str.drop_prefix(offset), is_not_newline) + 1;
        break;
      }
      case 'v': {
        // StringRef str = orig_str.drop_prefix(offset);
        // if (str.startswith("v ")) {
        //   str = str.drop_prefix(1);

        //   std::pair<uint, uint> span1 = find_next_word_in_line(str);
        //   StringRef str1 = str.substr(span1.first, span1.second);
        //   str = str.drop_prefix(span1.first + span1.second);

        //   std::pair<uint, uint> span2 = find_next_word_in_line(str);
        //   StringRef str2 = str.substr(span2.first, span2.second);
        //   str = str.drop_prefix(span2.first + span2.second);

        //   std::pair<uint, uint> span3 = find_next_word_in_line(str);
        //   StringRef str3 = str.substr(span3.first, span3.second);
        // }
        // else if (str.startswith("vt")) {
        //   /* TODO */
        // }
        // else if (str.startswith("vn")) {
        //   /* TODO */
        // }
        // offset += count_while(orig_str.drop_prefix(offset), is_not_newline) + 1;
        break;
      }
      default: {
        break;
      }
    }
  }

  return segments;
}

static void import_obj(bContext *UNUSED(C), StringRef file_path)
{
  std::ifstream input_stream;
  input_stream.open(file_path, std::ios::binary);

  TextLinesReader reader(input_stream);

  while (!reader.eof()) {
    StringRef text = reader.read_next_line_chunk(200);
    parse_obj_lines(text);
    reader.free_chunk(text);
  }

  //   Main *bmain = CTX_data_main(C);
  //   Collection *collection = CTX_data_collection(C);
  //   Mesh *mesh = BKE_mesh_add(bmain, "My Mesh");
  //   Object *object = BKE_object_add_only_object(bmain, OB_MESH, "My Object");
  //   object->data = mesh;
  //   BKE_collection_object_add(bmain, collection, object);
}

static int obj_import_exec(bContext *C, wmOperator *UNUSED(op))
{
  char filepath[FILE_MAX];
  strcpy(filepath, "/home/jacques/Documents/icosphere.obj");
  //   RNA_string_get(op->ptr, "filepath", filepath);
  std::cout << "Open: " << filepath << '\n';
  import_obj(C, filepath);
  return OPERATOR_FINISHED;
}

static int obj_import_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
  //   return WM_operator_filesel(C, op, event);
  return obj_import_exec(C, op);
}

void OBJECT_OT_obj_import_test(wmOperatorType *ot)
{
  ot->name = "Obj Import Test";
  ot->description = "Obj Import test";
  ot->idname = "OBJECT_OT_obj_import_test";

  ot->invoke = obj_import_invoke;
  ot->exec = obj_import_exec;

  /* Properties.*/
  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_OBJECT_IO,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_ALPHA);
}
