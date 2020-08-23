/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup obj
 */

#include <fstream>
#include <iostream>

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "wavefront_obj_ex_file_writer.hh"
#include "wavefront_obj_im_file_reader.hh"

namespace blender::io::obj {

using std::string;

/**
 * Store multiple lines separated by an escaped newline character: `\\n`.
 */
static void read_next_line(std::ifstream &file, string &r_line)
{
  std::string new_line;
  while (file.good() && r_line.back() == '\\') {
    new_line.clear();
    bool ok = static_cast<bool>(std::getline(file, new_line));
    /* Remove the last backslash character. */
    r_line.pop_back();
    r_line.append(new_line);
    if (!ok || new_line.empty()) {
      return;
    }
  }
}

/**
 * Split a line string into the first word (key) and the rest of the line.
 * Also remove leading & trailing spaces as well as `\r` carriage return
 * character if present.
 */
static void split_line_key_rest(StringRef line, StringRef &r_line_key, StringRef &r_rest_line)
{
  if (line.is_empty()) {
    return;
  }

  const int64_t pos_split{line.find_first_of(' ')};
  if (pos_split == StringRef::not_found) {
    /* Use the first character if no space is found in the line. It's usually a comment like:
     * #This is a comment. */
    r_line_key = line.substr(0, 1);
  }
  else {
    r_line_key = line.substr(0, pos_split);
  }

  r_rest_line = line = line.drop_prefix(r_line_key.size());
  if (r_rest_line.is_empty()) {
    return;
  }

  /* Remove any leading spaces, trailing spaces & \r character, if any. */
  const int64_t leading_space{r_rest_line.find_first_not_of(' ')};
  if (leading_space != StringRef::not_found) {
    r_rest_line = r_rest_line.drop_prefix(leading_space);
  }

  /* Another way is to do a test run before the actual parsing to find the newline
   * character and use it in the getline. */
  const int64_t carriage_return{r_rest_line.find_first_of('\r')};
  if (carriage_return != StringRef::not_found) {
    r_rest_line = r_rest_line.drop_prefix(carriage_return);
  }

  const int64_t trailing_space{r_rest_line.find_last_not_of(' ')};
  if (trailing_space != StringRef::not_found) {
    /* The position is of a character that is not ' ', so count of characters is position + 1. */
    r_rest_line = r_rest_line.substr(0, trailing_space + 1);
  }
}

/**
 * Split the given string by the delimiter and fill the given vector.
 * If an intermediate string is empty, or space or null character, it is not appended to the
 * vector.
 */
static void split_by_char(StringRef in_string, const char delimiter, Vector<StringRef> &r_out_list)
{
  r_out_list.clear();

  while (!in_string.is_empty()) {
    const int64_t pos_delim{in_string.find_first_of(delimiter)};
    const int64_t word_len = pos_delim == StringRef::not_found ? in_string.size() : pos_delim;

    StringRef word{in_string.data(), word_len};
    if (!word.is_empty() && !(word == " " && !(word[0] == '\0'))) {
      r_out_list.append(word);
    }
    if (pos_delim == StringRef::not_found) {
      return;
    }
    /* Skip the word already stored. */
    in_string = in_string.drop_prefix(word_len);
    /* Skip all delimiters. */
    in_string = in_string.drop_prefix(
        MIN2(in_string.find_first_not_of(delimiter), in_string.size()));
  }
}

/**
 * Convert the given string to float and assign it to the destination value.
 *
 * Catches exception if the string cannot be converted to a float. The destination value
 * is set to the given fallback value in that case.
 */

void copy_string_to_float(StringRef src, const float fallback_value, float &r_dst)
{
  try {
    r_dst = std::stof(string(src));
  }
  catch (const std::invalid_argument &inv_arg) {
    std::cerr << "Bad conversion to float:'" << inv_arg.what() << "':'" << src << "'" << std::endl;
    r_dst = fallback_value;
  }
  catch (const std::out_of_range &out_of_range) {
    std::cerr << "Out of range for float:'" << out_of_range.what() << ":'" << src << "'"
              << std::endl;
    r_dst = fallback_value;
  }
}

/**
 * Convert all members of the Span of strings to floats and assign them to the float
 * array members. Usually used for values like coordinates.
 *
 * Catches exception if any string cannot be converted to a float. The destination
 * float is set to the given fallback value in that case.
 */
BLI_INLINE void copy_string_to_float(Span<StringRef> src,
                                     const float fallback_value,
                                     MutableSpan<float> r_dst)
{
  BLI_assert(src.size() >= r_dst.size());
  for (int i = 0; i < r_dst.size(); ++i) {
    copy_string_to_float(src[i], fallback_value, r_dst[i]);
  }
}

/**
 * Convert the given string to int and assign it to the destination value.
 *
 * Catches exception if the string cannot be converted to an integer. The destination
 * int is set to the given fallback value in that case.
 */
BLI_INLINE void copy_string_to_int(StringRef src, const int fallback_value, int &r_dst)
{
  try {
    r_dst = std::stoi(string(src));
  }
  catch (const std::invalid_argument &inv_arg) {
    std::cerr << "Bad conversion to int:'" << inv_arg.what() << "':'" << src << "'" << std::endl;
    r_dst = fallback_value;
  }
  catch (const std::out_of_range &out_of_range) {
    std::cerr << "Out of range for int:'" << out_of_range.what() << ":'" << src << "'"
              << std::endl;
    r_dst = fallback_value;
  }
}

/**
 * Convert the given strings to ints and fill the destination int buffer.
 *
 * Catches exception if any string cannot be converted to an integer. The destination
 * int is set to the given fallback value in that case.
 */
BLI_INLINE void copy_string_to_int(Span<StringRef> src,
                                   const int fallback_value,
                                   MutableSpan<int> r_dst)
{
  BLI_assert(src.size() >= r_dst.size());
  for (int i = 0; i < r_dst.size(); ++i) {
    copy_string_to_int(src[i], fallback_value, r_dst[i]);
  }
}

/**
 * Based on the properties of the given Geometry instance, create a new Geometry instance
 * or return the previous one.
 *
 * Also update index offsets which should always happen if a new Geometry instance is created.
 */
static Geometry *create_geometry(Geometry *const prev_geometry,
                                 const eGeometryType new_type,
                                 StringRef name,
                                 const GlobalVertices &global_vertices,
                                 Vector<std::unique_ptr<Geometry>> &r_all_geometries,
                                 VertexIndexOffset &r_offset)
{
  auto new_geometry = [&]() {
    if (name.is_empty()) {
      r_all_geometries.append(std::make_unique<Geometry>(new_type, "New object"));
    }
    else {
      r_all_geometries.append(std::make_unique<Geometry>(new_type, name));
    }
    r_offset.set_index_offset(global_vertices.vertices.size());
    return r_all_geometries.last().get();
  };

  if (prev_geometry && prev_geometry->get_geom_type() == GEOM_MESH) {
    /* After the creation of a Geometry instance, at least one element has been found in the OBJ
     * file that indicates that it is a mesh. */
    if (prev_geometry->tot_verts() || prev_geometry->tot_face_elems() ||
        prev_geometry->tot_normals() || prev_geometry->tot_edges()) {
      return new_geometry();
    }
    if (new_type == GEOM_MESH) {
      /* A Geometry created initially with a default name now found its name. */
      prev_geometry->set_geometry_name(name);
      return prev_geometry;
    }
    if (new_type == GEOM_CURVE) {
      /* The object originally created is not a mesh now that curve data
       * follows the vertex coordinates list. */
      prev_geometry->set_geom_type(GEOM_CURVE);
      return prev_geometry;
    }
  }

  if (prev_geometry && prev_geometry->get_geom_type() == GEOM_CURVE) {
    return new_geometry();
  }

  return new_geometry();
}

/**
 * Open OBJ file at the path given in import parameters.
 */
OBJParser::OBJParser(const OBJImportParams &import_params) : import_params_(import_params)
{
  obj_file_.open(import_params_.filepath);
  if (!obj_file_.good()) {
    fprintf(stderr, "Cannot read from OBJ file:%s.\n", import_params_.filepath);
    return;
  }
}

/**
 * Read the OBJ file line by line and create OBJ Geometry instances. Also store all the vertex
 * and UV vertex coordinates in a struct accessible by all objects.
 */
void OBJParser::parse_and_store(Vector<std::unique_ptr<Geometry>> &r_all_geometries,
                                GlobalVertices &r_global_vertices)
{
  if (!obj_file_.good()) {
    return;
  }

  string line;
  /* Store vertex coordinates that belong to other Geometry instances.  */
  VertexIndexOffset offset;
  /* Non owning raw pointer to a Geometry.
   * Needed to update object data in the same while loop. */
  Geometry *current_geometry = create_geometry(
      nullptr, GEOM_MESH, "", r_global_vertices, r_all_geometries, offset);

  /* State-setting variables: if set, they remain the same for the remaining
   * elements in the object. */
  bool shaded_smooth = false;
  string object_group{};

  while (std::getline(obj_file_, line)) {
    /* Keep reading new lines if the last character is `\`. */
    /* Another way is to make a getline wrapper and use it in the while condition. */
    read_next_line(obj_file_, line);

    StringRef line_key, rest_line;
    split_line_key_rest(line, line_key, rest_line);
    if (line.empty() || rest_line.is_empty()) {
      continue;
    }

    if (line_key == "mtllib") {
      mtl_libraries_.append(string(rest_line));
    }
    else if (line_key == "o") {
      shaded_smooth = false;
      object_group = {};
      current_geometry = create_geometry(
          current_geometry, GEOM_MESH, rest_line, r_global_vertices, r_all_geometries, offset);
    }
    else if (line_key == "v") {
      BLI_assert(current_geometry);
      float3 curr_vert{};
      Vector<StringRef> str_vert_split;
      split_by_char(rest_line, ' ', str_vert_split);
      copy_string_to_float(str_vert_split, FLT_MAX, {curr_vert, 3});
      r_global_vertices.vertices.append(curr_vert);
      current_geometry->vertex_indices_.append(r_global_vertices.vertices.size() - 1);
    }
    else if (line_key == "vn") {
      float3 curr_vert_normal{};
      Vector<StringRef> str_vert_normal_split;
      split_by_char(rest_line, ' ', str_vert_normal_split);
      copy_string_to_float(str_vert_normal_split, FLT_MAX, {curr_vert_normal, 2});
      r_global_vertices.vertex_normals.append(curr_vert_normal);
      current_geometry->vertex_normal_indices_.append(r_global_vertices.vertex_normals.size() - 1);
    }
    else if (line_key == "vt") {
      float2 curr_uv_vert{};
      Vector<StringRef> str_uv_vert_split;
      split_by_char(rest_line, ' ', str_uv_vert_split);
      copy_string_to_float(str_uv_vert_split, FLT_MAX, {curr_uv_vert, 2});
      r_global_vertices.uv_vertices.append(curr_uv_vert);
    }
    else if (line_key == "l") {
      BLI_assert(current_geometry);
      int edge_v1 = -1, edge_v2 = -1;
      Vector<StringRef> str_edge_split;
      split_by_char(rest_line, ' ', str_edge_split);
      copy_string_to_int(str_edge_split[0], -1, edge_v1);
      copy_string_to_int(str_edge_split[1], -1, edge_v2);
      /* Always keep stored indices non-negative and zero-based. */
      edge_v1 += edge_v1 < 0 ? r_global_vertices.vertices.size() : -offset.get_index_offset() - 1;
      edge_v2 += edge_v2 < 0 ? r_global_vertices.vertices.size() : -offset.get_index_offset() - 1;
      BLI_assert(edge_v1 >= 0 && edge_v2 >= 0);
      current_geometry->edges_.append({static_cast<uint>(edge_v1), static_cast<uint>(edge_v2)});
    }
    else if (line_key == "g") {
      object_group = rest_line;
      if (object_group.find("off") != string::npos || object_group.find("null") != string::npos ||
          object_group.find("default") != string::npos) {
        /* Set group for future elements like faces or curves to empty. */
        object_group = {};
      }
    }
    else if (line_key == "s") {
      /* Some implementations use "0" and "null" too, in addition to "off". */
      if (rest_line != "0" && rest_line.find("off") == StringRef::not_found &&
          rest_line.find("null") == StringRef::not_found) {
        int smooth = 0;
        copy_string_to_int(rest_line, 0, smooth);
        shaded_smooth = smooth != 0;
      }
      else {
        /* The OBJ file explicitly set shading to off. */
        shaded_smooth = false;
      }
    }
    else if (line_key == "f") {
      BLI_assert(current_geometry);
      FaceElement curr_face;
      curr_face.shaded_smooth = shaded_smooth;
      if (!object_group.empty()) {
        curr_face.vertex_group = object_group;
        /* Yes it repeats several times, but another if-check will not reduce steps either. */
        current_geometry->use_vertex_groups_ = true;
      }

      Vector<StringRef> str_corners_split;
      split_by_char(rest_line, ' ', str_corners_split);
      for (StringRef str_corner : str_corners_split) {
        FaceCorner corner;
        size_t n_slash = std::count(str_corner.begin(), str_corner.end(), '/');
        if (n_slash == 0) {
          /* Case: f v1 v2 v3 . */
          copy_string_to_int(str_corner, INT32_MAX, corner.vert_index);
        }
        else if (n_slash == 1) {
          /* Case: f v1/vt1 v2/vt2 v3/vt3 . */
          Vector<StringRef> vert_uv_split;
          split_by_char(str_corner, '/', vert_uv_split);
          copy_string_to_int(vert_uv_split[0], INT32_MAX, corner.vert_index);
          if (vert_uv_split.size() == 2) {
            copy_string_to_int(vert_uv_split[1], INT32_MAX, corner.uv_vert_index);
          }
        }
        else if (n_slash == 2) {
          /* Case: f v1//vn1 v2//vn2 v3//vn3 . */
          /* Case: f v1/vt1/vn1 v2/vt2/vn2 v3/vt3/vn3 . */
          Vector<StringRef> vert_uv_normal_split{};
          split_by_char(str_corner, '/', vert_uv_normal_split);
          copy_string_to_int(vert_uv_normal_split[0], INT32_MAX, corner.vert_index);
          if (vert_uv_normal_split.size() == 3) {
            copy_string_to_int(vert_uv_normal_split[1], INT32_MAX, corner.uv_vert_index);
          }
          copy_string_to_int(vert_uv_normal_split[2], INT32_MAX, corner.vertex_normal_index);
        }
        /* Always keep stored indices non-negative and zero-based. */
        corner.vert_index += corner.vert_index < 0 ? r_global_vertices.vertices.size() :
                                                     -offset.get_index_offset() - 1;
        corner.uv_vert_index += corner.uv_vert_index < 0 ? r_global_vertices.uv_vertices.size() :
                                                           -1;
        corner.vertex_normal_index += corner.vertex_normal_index < 0 ?
                                          r_global_vertices.vertex_normals.size() :
                                          -1;
        curr_face.face_corners.append(corner);
      }

      current_geometry->face_elements_.append(curr_face);
      current_geometry->tot_loops_ += curr_face.face_corners.size();
    }
    else if (line_key == "cstype") {
      if (rest_line.find("bspline") != StringRef::not_found) {
        current_geometry = create_geometry(current_geometry,
                                           GEOM_CURVE,
                                           object_group,
                                           r_global_vertices,
                                           r_all_geometries,
                                           offset);
        current_geometry->nurbs_element_.group_ = object_group;
      }
      else {
        std::cerr << "Curve type not supported:'" << rest_line << "'" << std::endl;
      }
    }
    else if (line_key == "deg") {
      copy_string_to_int(rest_line, 3, current_geometry->nurbs_element_.degree);
    }
    else if (line_key == "curv") {
      Vector<StringRef> str_curv_split;
      split_by_char(rest_line, ' ', str_curv_split);
      /* Remove "0.0" and "1.0" from the strings. They are hardcoded. */
      str_curv_split.remove(0);
      str_curv_split.remove(0);
      current_geometry->nurbs_element_.curv_indices.resize(str_curv_split.size());
      copy_string_to_int(str_curv_split, INT32_MAX, current_geometry->nurbs_element_.curv_indices);
      for (int &curv_index : current_geometry->nurbs_element_.curv_indices) {
        /* Always keep stored indices non-negative and zero-based. */
        curv_index += curv_index < 0 ? r_global_vertices.vertices.size() : -1;
      }
    }
    else if (line_key == "parm") {
      Vector<StringRef> str_parm_split;
      split_by_char(rest_line, ' ', str_parm_split);
      if (str_parm_split[0] == "u" || str_parm_split[0] == "v") {
        str_parm_split.remove(0);
        current_geometry->nurbs_element_.parm.resize(str_parm_split.size());
        copy_string_to_float(str_parm_split, FLT_MAX, current_geometry->nurbs_element_.parm);
      }
      else {
        std::cerr << "Surfaces are not supported:'" << str_parm_split[0] << "'" << std::endl;
      }
    }
    else if (line_key == "end") {
      /* Curves mark their end this way. */
    }
    else if (line_key == "usemtl") {
      current_geometry->material_names_.append(string(rest_line));
    }
  }
}

/**
 * Skip all texture map options and get the filepath from a "map_" line.
 */
static StringRef skip_unsupported_options(StringRef line)
{
  TextureMapOptions map_options;
  StringRef last_option;
  int64_t last_option_pos = 0;

  /* Find the last texture map option. */
  for (StringRef option : map_options.all_options()) {
    const int64_t pos{line.find(option)};
    /* Equality (>=) takes care of finding an option in the beginning of the line. Avoid messing
     * with signed-unsigned int comparison. */
    if (pos != StringRef::not_found && pos >= last_option_pos) {
      last_option = option;
      last_option_pos = pos;
    }
  }

  if (last_option.is_empty()) {
    /* No option found, line is the filepath */
    return line;
  }

  /* Remove upto start of the last option + size of the last option + space after it. */
  line = line.drop_prefix(last_option_pos + last_option.size() + 1);
  for (int i = 0; i < map_options.number_of_args(last_option); i++) {
    int64_t pos_space{line.find_first_of(' ')};
    if (pos_space != StringRef::not_found) {
      BLI_assert(pos_space + 1 < line.size());
      line = line.drop_prefix(pos_space + 1);
    }
  }

  return line;
}

/**
 * Return a list of all material library filepaths referenced by the OBJ file.
 */
Span<std::string> OBJParser::mtl_libraries() const
{
  return mtl_libraries_;
}

/**
 * Open material library file.
 */
MTLParser::MTLParser(StringRef mtl_library, StringRefNull obj_filepath)
{
  char obj_file_dir[FILE_MAXDIR];
  BLI_split_dir_part(obj_filepath.data(), obj_file_dir, FILE_MAXDIR);
  BLI_path_join(mtl_file_path_, FILE_MAX, obj_file_dir, mtl_library.data(), NULL);
  BLI_split_dir_part(mtl_file_path_, mtl_dir_path_, FILE_MAXDIR);
  mtl_file_.open(mtl_file_path_);
  if (!mtl_file_.good()) {
    fprintf(stderr, "Cannot read from MTL file:%s\n", mtl_file_path_);
  }
}

/**
 * Read MTL file(s) and add MTLMaterial instances to the given Map reference.
 */
void MTLParser::parse_and_store(Map<string, MTLMaterial> &r_mtl_materials)
{
  if (!mtl_file_.good()) {
    return;
  }

  string line;
  MTLMaterial *current_mtlmaterial = nullptr;

  while (std::getline(mtl_file_, line)) {
    StringRef line_key{}, rest_line{};
    split_line_key_rest(line, line_key, rest_line);
    if (line.empty() || rest_line.is_empty()) {
      continue;
    }

    if (line_key == "newmtl") {
      if (r_mtl_materials.remove_as(rest_line)) {
        std::cerr << "Duplicate material found:'" << rest_line << "'." << std::endl;
      }
      current_mtlmaterial = &r_mtl_materials.lookup_or_add_default_as(string(rest_line));
    }
    else if (line_key == "Ns") {
      copy_string_to_float(rest_line, 324.0f, current_mtlmaterial->Ns);
    }
    else if (line_key == "Ka") {
      Vector<StringRef> str_ka_split{};
      split_by_char(rest_line, ' ', str_ka_split);
      copy_string_to_float(str_ka_split, 0.0f, {current_mtlmaterial->Ka, 3});
    }
    else if (line_key == "Kd") {
      Vector<StringRef> str_kd_split{};
      split_by_char(rest_line, ' ', str_kd_split);
      copy_string_to_float(str_kd_split, 0.8f, {current_mtlmaterial->Kd, 3});
    }
    else if (line_key == "Ks") {
      Vector<StringRef> str_ks_split{};
      split_by_char(rest_line, ' ', str_ks_split);
      copy_string_to_float(str_ks_split, 0.5f, {current_mtlmaterial->Ks, 3});
    }
    else if (line_key == "Ke") {
      Vector<StringRef> str_ke_split{};
      split_by_char(rest_line, ' ', str_ke_split);
      copy_string_to_float(str_ke_split, 0.0f, {current_mtlmaterial->Ke, 3});
    }
    else if (line_key == "Ni") {
      copy_string_to_float(rest_line, 1.45f, current_mtlmaterial->Ni);
    }
    else if (line_key == "d") {
      copy_string_to_float(rest_line, 1.0f, current_mtlmaterial->d);
    }
    else if (line_key == "illum") {
      copy_string_to_int(rest_line, 2, current_mtlmaterial->illum);
    }

    /* Parse image textures. */
    else if (line_key.find("map_") != StringRef::not_found) {
      if (!current_mtlmaterial->texture_maps.contains_as(string(line_key))) {
        /* No supported texture map found. */
        std::cerr << "Texture map type not supported:'" << line_key << "'" << std::endl;
        continue;
      }
      tex_map_XX &tex_map = current_mtlmaterial->texture_maps.lookup(string(line_key));
      Vector<StringRef> str_map_xx_split{};
      split_by_char(rest_line, ' ', str_map_xx_split);

      /* TODO ankitm: use `skip_unsupported_options` for parsing these options too? */
      const int64_t pos_o{str_map_xx_split.first_index_of_try("-o")};
      if (pos_o != -1 && pos_o + 3 < str_map_xx_split.size()) {
        copy_string_to_float({str_map_xx_split[pos_o + 1],
                              str_map_xx_split[pos_o + 2],
                              str_map_xx_split[pos_o + 3]},
                             0.0f,
                             {tex_map.translation, 3});
      }
      const int64_t pos_s{str_map_xx_split.first_index_of_try("-s")};
      if (pos_s != -1 && pos_s + 3 < str_map_xx_split.size()) {
        copy_string_to_float({str_map_xx_split[pos_s + 1],
                              str_map_xx_split[pos_s + 2],
                              str_map_xx_split[pos_s + 3]},
                             1.0f,
                             {tex_map.scale, 3});
      }
      /* Only specific to Normal Map node. */
      const int64_t pos_bm{str_map_xx_split.first_index_of_try("-bm")};
      if (pos_bm != -1 && pos_bm + 1 < str_map_xx_split.size()) {
        copy_string_to_float(
            str_map_xx_split[pos_bm + 1], 0.0f, current_mtlmaterial->map_Bump_strength);
      }

      /* Skip all unsupported options and arguments. */
      tex_map.image_path = string(skip_unsupported_options(rest_line));
      tex_map.mtl_dir_path = mtl_dir_path_;
    }
  }
}
}  // namespace blender::io::obj