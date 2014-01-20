/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <xenia/gpu/d3d11/d3d11_geometry_shader.h>

#include <xenia/gpu/gpu-private.h>
#include <xenia/gpu/d3d11/d3d11_shader.h>
#include <xenia/gpu/xenos/ucode.h>

#include <d3dcompiler.h>


using namespace xe;
using namespace xe::gpu;
using namespace xe::gpu::d3d11;
using namespace xe::gpu::xenos;


D3D11GeometryShader::D3D11GeometryShader(ID3D11Device* device, uint64_t hash) :
    hash_(hash), handle_(NULL) {
  device_ = device;
  device_->AddRef();
}

D3D11GeometryShader::~D3D11GeometryShader() {
  XESAFERELEASE(handle_);
  XESAFERELEASE(device_);
}

int D3D11GeometryShader::Prepare(D3D11VertexShader* vertex_shader) {
  if (handle_) {
    return 0;
  }

  // TODO(benvanik): look in file based on hash/etc.
  void* byte_code = NULL;
  size_t byte_code_length = 0;

  // Translate and compile source.
  auto output = new alloy::StringBuffer();
  if (Generate(vertex_shader, output)) {
    delete output;
    return 1;
  }
  ID3D10Blob* shader_blob = Compile(output->GetString());
  delete output;
  if (!shader_blob) {
    return 1;
  }
  byte_code_length = shader_blob->GetBufferSize();
  byte_code = xe_malloc(byte_code_length);
  xe_copy_struct(
      byte_code, shader_blob->GetBufferPointer(), byte_code_length);
  XESAFERELEASE(shader_blob);

  // Create shader.
  HRESULT hr = device_->CreateGeometryShader(
      byte_code, byte_code_length,
      NULL,
      &handle_);
  if (FAILED(hr)) {
    XELOGE("D3D11: failed to create geometry shader");
    xe_free(byte_code);
    return 1;
  }

  return 0;
}

ID3D10Blob* D3D11GeometryShader::Compile(const char* shader_source) {
  // TODO(benvanik): pick shared runtime mode defines.
  D3D10_SHADER_MACRO defines[] = {
    "TEST_DEFINE", "1",
    0, 0,
  };

  uint32_t flags1 = 0;
  flags1 |= D3D10_SHADER_DEBUG;
  flags1 |= D3D10_SHADER_ENABLE_STRICTNESS;
  uint32_t flags2 = 0;

  // Create a name.
  const char* base_path = "";
  if (FLAGS_dump_shaders.size()) {
    base_path = FLAGS_dump_shaders.c_str();
  }
  char file_name[XE_MAX_PATH];
  xesnprintfa(file_name, XECOUNT(file_name),
      "%s/gen_%.16llX.gs",
      base_path,
      hash_);

  if (FLAGS_dump_shaders.size()) {
    FILE* f = fopen(file_name, "w");
    fprintf(f, shader_source);
    fclose(f);
  }

  // Compile shader to bytecode blob.
  ID3D10Blob* shader_blob = 0;
  ID3D10Blob* error_blob = 0;
  HRESULT hr = D3DCompile(
      shader_source, strlen(shader_source),
      file_name,
      defines, NULL,
      "main",
      "gs_5_0",
      flags1, flags2,
      &shader_blob, &error_blob);
  if (error_blob) {
    char* msg = (char*)error_blob->GetBufferPointer();
    XELOGE("D3D11: shader compile failed with %s", msg);
  }
  XESAFERELEASE(error_blob);
  if (FAILED(hr)) {
    return NULL;
  }
  return shader_blob;
}

int D3D11GeometryShader::Generate(D3D11VertexShader* vertex_shader,
                                  alloy::StringBuffer* output) {
  output->Append(
    "struct VERTEX {\n"
    "  float4 oPos : SV_POSITION;\n");
  auto alloc_counts = vertex_shader->alloc_counts();
  if (alloc_counts.params) {
    // TODO(benvanik): only add used ones?
    output->Append(
      "  float4 o[%d] : XE_O;\n",
      D3D11Shader::MAX_INTERPOLATORS);
  }
  output->Append(
    // TODO(benvanik): only pull in point size if required.
    "  float4 oPointSize : PSIZE;\n"
    "};\n");

  output->Append(
    "cbuffer geo_consts {\n"
    "  float4 window;\n"              // x,y,w,h
    "  float4 viewport_z_enable;\n"   // min,(max - min),?,enabled
    "  float4 viewport_size;\n"       // x,y,w,h
    "};"
    "float4 applyViewport(float4 pos) {\n"
    "  if (viewport_z_enable.w) {\n"
    //"    pos.x = (pos.x + 1) * viewport_size.z * 0.5 + viewport_size.x;\n"
    //"    pos.y = (1 - pos.y) * viewport_size.w * 0.5 + viewport_size.y;\n"
    //"    pos.z = viewport_z_enable.x + pos.z * viewport_z_enable.y;\n"
    // w?
    "  } else {\n"
    "    pos.xy = pos.xy / float2(window.z / 2.0, -window.w / 2.0) + float2(-1.0, 1.0);\n"
    "    pos.zw = float2(0.0, 1.0);\n"
    "  }\n"
    "  pos.xy += window.xy;\n"
    "  return pos;\n"
    "}\n");

  return 0;
}


D3D11PointSpriteGeometryShader::D3D11PointSpriteGeometryShader(
    ID3D11Device* device, uint64_t hash) :
    D3D11GeometryShader(device, hash) {
}

D3D11PointSpriteGeometryShader::~D3D11PointSpriteGeometryShader() {
}

int D3D11PointSpriteGeometryShader::Generate(D3D11VertexShader* vertex_shader,
                                             alloy::StringBuffer* output) {
  if (D3D11GeometryShader::Generate(vertex_shader, output)) {
    return 1;
  }

  // TODO(benvanik): fetch default point size from register and use that if
  //     the VS doesn't write oPointSize.
  // TODO(benvanik): clamp to min/max.
  // TODO(benvanik): figure out how to see which interpolator gets adjusted.

  output->Append(
    "[maxvertexcount(4)]\n"
    "void main(point VERTEX input[1], inout TriangleStream<VERTEX> output) {\n"
    "  const float2 offsets[4] = {\n"
    "   float2(-1.0,  1.0),\n"
    "   float2( 1.0,  1.0),\n"
    "   float2(-1.0, -1.0),\n"
    "   float2( 1.0, -1.0),\n"
    "  };\n"
    "  float psize = max(input[0].oPointSize.x, 1.0);\n"
    "  for (uint n = 0; n < 4; n++) {\n"
    "    VERTEX v = input[0];\n"
    "    v.oPos.xy += offsets[n] * psize;\n"
    "    output.Append(v);\n"
    "  }\n"
    "  output.RestartStrip();\n"
    "}\n");

  return 0;
}


D3D11RectListGeometryShader::D3D11RectListGeometryShader(
    ID3D11Device* device, uint64_t hash) :
    D3D11GeometryShader(device, hash) {
}

D3D11RectListGeometryShader::~D3D11RectListGeometryShader() {
}

int D3D11RectListGeometryShader::Generate(D3D11VertexShader* vertex_shader,
                                          alloy::StringBuffer* output) {
  if (D3D11GeometryShader::Generate(vertex_shader, output)) {
    return 1;
  }

  output->Append(
    "[maxvertexcount(4)]\n"
    "void main(triangle VERTEX input[3], inout TriangleStream<VERTEX> output) {\n"
    "  for (uint n = 0; n < 3; n++) {\n"
    "    VERTEX v = input[n];\n"
    "    v.oPos = applyViewport(v.oPos);\n"
    "    output.Append(v);\n"
    "  }\n"
    "  VERTEX v = input[2];\n"
    "  v.oPos = applyViewport(v.oPos + input[1].oPos - input[0].oPos);\n"
    // TODO(benvanik): only if needed?
    "  v.oPointSize += input[1].oPointSize - input[0].oPointSize;\n");
  auto alloc_counts = vertex_shader->alloc_counts();
  for (uint32_t n = 0; n < alloc_counts.params; n++) {
    // TODO(benvanik): this may be wrong - the count is a bad metric.
    output->Append(
      "  v.o[%d] += input[1].o[%d] - input[0].o[%d];\n",
      n, n, n, n);
  }
  output->Append(
    "  output.Append(v);\n"
    "  output.RestartStrip();\n"
    "}\n");

  return 0;
}


D3D11QuadListGeometryShader::D3D11QuadListGeometryShader(
    ID3D11Device* device, uint64_t hash) :
    D3D11GeometryShader(device, hash) {
}

D3D11QuadListGeometryShader::~D3D11QuadListGeometryShader() {
}

int D3D11QuadListGeometryShader::Generate(D3D11VertexShader* vertex_shader,
                                          alloy::StringBuffer* output) {
  if (D3D11GeometryShader::Generate(vertex_shader, output)) {
    return 1;
  }

  output->Append(
    "[maxvertexcount(4)]\n"
    "void main(lineadj VERTEX input[4], inout TriangleStream<VERTEX> output) {\n"
    "  const uint order[4] = { 0, 1, 3, 2 };\n"
    "  for (uint n = 0; n < 4; n++) {\n"
    "    VERTEX v = input[order[n]];\n"
    "    v.oPos = applyViewport(v.oPos);\n"
    "    output.Append(v);\n"
    "  }\n"
    "  output.RestartStrip();\n"
    "}\n");

  return 0;
}