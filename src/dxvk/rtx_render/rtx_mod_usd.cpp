/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#pragma once

#include "rtx_mod_usd.h"
#include "rtx_asset_replacer.h"

#include "dxvk_device.h"
#include "dxvk_context.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "rtx_game_capturer_paths.h"
#include "rtx_utils.h"
#include "rtx_asset_data_manager.h"

#include "../../lssusd/usd_include_begin.h"
#include <pxr/base/gf/matrix4f.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include "pxr/usd/usdLux/lightapi.h"
#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/base/arch/fileSystem.h>
#include "../../lssusd/usd_include_end.h"
#include "../util/util_watchdog.h"

#include "../../lssusd/usd_mesh_importer.h"
#include "../../lssusd/usd_common.h"

namespace fs = std::filesystem;

namespace dxvk {
constexpr uint32_t kMaxU16Indices = 64 * 1024;
const char* const kStatusKey = "remix_replacement_status";

class UsdMod::Impl {
public:
  Impl(UsdMod& owner) 
    : m_owner{owner}
    , m_usdChangeWatchdog([this] { return this->haveFilesChanged(); }, "usd-mod-watchdog")
  {}

  void load(const Rc<DxvkContext>& context);
  void unload();
  bool checkForChanges(const Rc<DxvkContext>& context);

private:
  UsdMod& m_owner;

  struct Args {
    Rc<DxvkContext> context;
    pxr::UsdGeomXformCache& xformCache;

    pxr::UsdPrim& rootPrim;
    std::vector<AssetReplacement>& meshes;
  };

  bool haveFilesChanged();

  void processUSD(const Rc<DxvkContext>& context);

  void TEMP_parseSecretReplacementVariants(const fast_unordered_cache<uint32_t>& variants);
  Rc<ManagedTexture> getTexture(const Args& args, const pxr::UsdPrim& shader, const pxr::TfToken& textureToken, bool forcePreload = false);
  MaterialData* processMaterial(Args& args, const pxr::UsdPrim& matPrim);
  MaterialData* processMaterialUser(Args& args, const pxr::UsdPrim& prim);
  bool processMesh(const pxr::UsdPrim& prim, Args& args);
  void processPrim(Args& args, pxr::UsdPrim& prim);

  void processLight(Args& args, const pxr::UsdPrim& lightPrim);
  void processReplacement(Args& args);

  Categorizer processCategoryFlags(const pxr::UsdPrim& prim);

  // Returns next hash value compatible with geometry and drawcall hashing
  XXH64_hash_t getNextGeomHash() {
    static size_t id;
    ++id;
    return XXH64(&id, sizeof(id), 0);
  }

  std::filesystem::file_time_type m_fileModificationTime;
  std::string m_openedFilePath;

  Watchdog<1000> m_usdChangeWatchdog;
};

// context and member variable arguments to pass down to anonymous functions (to avoid having USD in the header)

namespace {
// Find the first prim in the layer stack that has a non-xform or material binding attribute
// return the hash of the filename and prim path.
XXH64_hash_t getStrongestOpinionatedPathHash(const pxr::UsdPrim& prim) {
  static const char* kXformPrefix = "xform";
  static const size_t kXformLen = strlen(kXformPrefix);
  static const pxr::TfToken kMaterialBinding("material:binding");
  auto stack = prim.GetPrimStack();
  for (auto spec : stack) {
    for (auto property : spec->GetProperties()) {
      if (property->GetName().compare(0, kXformLen, kXformPrefix) == 0) {
        // xform property
        continue;
      } else if (property->GetNameToken() == kMaterialBinding) {
        //material binding
        continue;
      }
      // This is the primSpec to use
      std::string originOfMeshFile = spec->GetLayer()->GetRealPath();
      std::string originPath = spec->GetPath().GetString();

      XXH64_hash_t usdOriginHash = 0;
      usdOriginHash = XXH64(originOfMeshFile.c_str(), originOfMeshFile.size(), usdOriginHash);
      usdOriginHash = XXH64(originPath.c_str(), originPath.size(), usdOriginHash);

      return usdOriginHash;
    }
  }
  Logger::err(str::format("Asset Replacement failed to find a source prim for ", prim.GetPath().GetString()));
  // fall back to using the prim's path in replacements.usda.  Potentially worse performance, since it may lead to duplicates.
  std::string name = prim.GetPath().GetString();
  return XXH3_64bits(name.c_str(), name.size());
}

XXH64_hash_t getNamedHash(const std::string& name, const char* prefix, const size_t len) {
  if (name.compare(0, len, prefix) == 0) {
    // is a mesh replacement.
    return std::strtoull(name.c_str()+len, nullptr, 16);
  } else {
    // Not a mesh replacements
    return 0;
  }
}

XXH64_hash_t getModelHash(const pxr::UsdPrim& prim) {
  static const char* prefix = lss::prefix::mesh.c_str();
  static const size_t len = strlen(prefix);
  return getNamedHash(prim.GetName().GetString(), prefix, len);
}

XXH64_hash_t getLightHash(const pxr::UsdPrim& prim) {
  static const char* prefix = lss::prefix::light.c_str();
  static const size_t len = strlen(prefix);
  if (prim.GetName().GetText()[0] == 's') {
    // Handling for legacy `sphereLight_HASH` names.  TODO Remove once assets are updated
    static const char* legacyPrefix = "sphereLight_";
    static const size_t legacyLen = strlen(legacyPrefix);
    return getNamedHash(prim.GetName().GetString(), legacyPrefix, legacyLen);
  }
  return getNamedHash(prim.GetName().GetString(), prefix, len);
}

XXH64_hash_t getMaterialHash(const pxr::UsdPrim& prim, const pxr::UsdPrim& shader) {
  static const pxr::TfToken kMaterialType("Material");
  static const char* prefix = lss::prefix::mat.c_str();
  static const size_t len = strlen(prefix);
  std::string name = prim.GetName().GetString();
  XXH64_hash_t nameHash = getNamedHash(name, prefix, len);
  if (nameHash != 0) {
    return nameHash;
  }
  if (prim.GetTypeName() != kMaterialType) {
    return 0;
  }

  if (!shader.IsValid()) {
    return 0;
  }
  
  XXH64_hash_t usdOriginHash = getStrongestOpinionatedPathHash(shader);

  return usdOriginHash;
}

bool getVector3(const pxr::UsdAttribute& attr, Vector3& vector) {
    if (attr.HasValue()) {
      pxr::GfVec3f vec;
      attr.Get(&vec);
      vector = Vector3(vec.data());
      return true;
    }
    return false;
}

bool getVector3(const pxr::UsdPrim& prim, const pxr::TfToken& token, Vector3& vector) {
  return getVector3(prim.GetAttribute(token), vector);
}

// USD transitioned from `intensity` to `inputs:intensity` for all its light attributes, we need to support content
// authored before and after that change.
const pxr::UsdAttribute getLightAttribute(const pxr::UsdPrim& prim, const pxr::TfToken& token, const pxr::TfToken& inputToken) {
  const pxr::UsdAttribute& attr = prim.GetAttribute(inputToken);
  if (!attr.HasValue()) {
    const pxr::UsdAttribute& old_attr = prim.GetAttribute(token);
    if (old_attr.HasValue()) {
      ONCE(Logger::warn(str::format("Legacy light attribute detected: ", old_attr.GetPath())));
    }
    return old_attr;
  }
  return attr;
}

RtLightShaping getLightShaping(const pxr::UsdPrim& lightPrim, Vector3 zAxis) {
  static const pxr::TfToken kConeAngleToken("shaping:cone:angle");
  static const pxr::TfToken kConeSoftnessToken("shaping:cone:softness");
  static const pxr::TfToken kFocusToken("shaping:focus");
  static const pxr::TfToken kInputsConeAngleToken("inputs:shaping:cone:angle");
  static const pxr::TfToken kInputsConeSoftnessToken("inputs:shaping:cone:softness");
  static const pxr::TfToken kInputsFocusToken("inputs:shaping:focus");

  RtLightShaping shaping;

  shaping.primaryAxis = zAxis;

  float angle = 180.f;
  getLightAttribute(lightPrim, kConeAngleToken, kInputsConeAngleToken).Get(&angle);
  shaping.cosConeAngle = cos(angle * kDegreesToRadians);
  
  float softness = 0.0f;
  getLightAttribute(lightPrim, kConeSoftnessToken, kInputsConeSoftnessToken).Get(&softness);
  shaping.coneSoftness = softness;

  float focus = 0.0f;
  getLightAttribute(lightPrim, kFocusToken, kInputsFocusToken).Get(&focus);
  shaping.focusExponent = focus;
  
  if (shaping.cosConeAngle != -1.f || shaping.coneSoftness != 0.0f || shaping.focusExponent != 0.0f ) {
    shaping.enabled = true;
  }
  return shaping;
}
}  // namespace

// Resolves full path for a texture in a shader from texture USD asset path and source USD path.
// This method is used when real path to a texture asset was not resolved by USD, e.g. the asset
// is likely packaged and is not physically present on disk.
static std::string resolveTexturePath(
  const pxr::UsdPrim& shader,
  const pxr::TfToken& textureToken,
  const std::string& textureAssetPath) {
  for (auto spec : shader.GetPrimStack()) {
    auto attribs = spec->GetAttributes();
    if (attribs.find(textureToken) != attribs.end()) {
      std::filesystem::path sourcePath(spec->GetLayer()->GetRealPath());
      std::filesystem::path resolvedPath = sourcePath.parent_path();

      // Process special path symbols.
      // Note: we could use filesystem::weakly_canonical() to make the resulting path
      // canonical but unfortunately it is extremly expensive.
      size_t pathStartPos = 0;
      while (pathStartPos < textureAssetPath.size()) {
        // Check for current folder symbol
        if (textureAssetPath[pathStartPos] == '.') {
          // Skip it
          ++pathStartPos;
          // Check for parent folder symbol
          if (textureAssetPath[pathStartPos] == '.') {
            resolvedPath = resolvedPath.parent_path();
            ++pathStartPos;
          }
        } else if (textureAssetPath[pathStartPos] == '\\' || textureAssetPath[pathStartPos] == '/') {
          // Skip path separator
          ++pathStartPos;
        } else {
          break;
        }
      }

      resolvedPath /= textureAssetPath.data() + pathStartPos;
      resolvedPath.make_preferred();

      return resolvedPath.string();
    }
  }
  Logger::warn(str::format("Unable to resolve full path for ", textureAssetPath));
  return textureAssetPath;
}

Rc<ManagedTexture> UsdMod::Impl::getTexture(const Args& args, const pxr::UsdPrim& shader, const pxr::TfToken& textureToken, bool forcePreload) {
  static const pxr::TfToken kSRGBColorSpace("sRGB");
  static pxr::SdfAssetPath path;
  auto attr = shader.GetAttribute(textureToken);
  if (attr.Get(&path)) {
    const ColorSpace colorSpace = ColorSpace::AUTO; // Always do this, whether or not force SRGB is required or not is unclear at this time.

    std::string resolvedTexturePath;
    if (!path.GetResolvedPath().empty()) {
      // We have a resolved path - texture file exists on disk
      resolvedTexturePath = path.GetResolvedPath();
    } else if (!path.GetAssetPath().empty()) {
      // We do NOT have a resolved path - this could be a packaged texture
      // Resolve full path from the asset path and source USD path
      resolvedTexturePath = resolveTexturePath(shader, textureToken, path.GetAssetPath());
    } else {
      // No texture set
      return nullptr;
    }

    auto assetData = AssetDataManager::get().findAsset(resolvedTexturePath);
    if (assetData != nullptr) {
      auto device = args.context->getDevice();
      auto& textureManager = device->getCommon()->getTextureManager();
      return textureManager.preloadTextureAsset(assetData, colorSpace, args.context, forcePreload);
    } else if (RtxOptions::Automation::suppressAssetLoadingErrors()) {
      Logger::warn(str::format("Texture ", resolvedTexturePath, " asset data cannot be found or corrupted."));
    } else {
      Logger::err(str::format("Texture ", resolvedTexturePath, " asset data cannot be found or corrupted."));
    }
  }

  // Note: "Empty" texture returned on failure
  return nullptr;
}

MaterialData* UsdMod::Impl::processMaterial(Args& args, const pxr::UsdPrim& matPrim) {
  ScopedCpuProfileZone();

  // Textures
  static const pxr::TfToken kShaderToken("Shader");
  static const pxr::TfToken kAlbedoTextureToken("inputs:diffuse_texture");
  static const pxr::TfToken kNormalTextureToken("inputs:normalmap_texture");
  static const pxr::TfToken kTangentTextureToken("inputs:tangent_texture");
  static const pxr::TfToken kHeightTextureToken("inputs:height_texture");
  static const pxr::TfToken kRoughnessTextureToken("inputs:reflectionroughness_texture");
  static const pxr::TfToken kMetallicTextureToken("inputs:metallic_texture");
  static const pxr::TfToken kEmissiveMaskTextureToken("inputs:emissive_mask_texture");
  // Attributes
  static const pxr::TfToken kIgnore("inputs:ignore_material");  // Any draw call or replacement using a material with this flag will be skipped by the SceneManager
  static const pxr::TfToken kAnisotropy("inputs:anisotropy");
  static const pxr::TfToken kEmissiveIntensity("inputs:emissive_intensity");
  static const pxr::TfToken kAlbedoConstant("inputs:diffuse_color_constant");
  static const pxr::TfToken kRoughnessConstant("inputs:reflection_roughness_constant");
  static const pxr::TfToken kMetallicConstant("inputs:metallic_constant");
  static const pxr::TfToken kEmissiveColorConstant("inputs:emissive_color_constant");
  static const pxr::TfToken kOpacityConstant("inputs:opacity_constant");

  static const pxr::TfToken kIORConstant("inputs:ior_constant");
  static const pxr::TfToken kEnableEmission("inputs:enable_emission");
  static const pxr::TfToken kEmissiveColor("inputs:emissive_color");
  static const pxr::TfToken kTransmittanceTexture("inputs:transmittance_texture");
  static const pxr::TfToken kTransmittanceConstant("inputs:transmittance_color");
  static const pxr::TfToken kTransmittanceDistanceConstant("inputs:transmittance_measurement_distance");
  static const pxr::TfToken kIsThinWalled("inputs:thin_walled");
  static const pxr::TfToken kThinWallThickness("inputs:thin_wall_thickness");
  static const pxr::TfToken kUseDiffuseLayer("inputs:use_diffuse_layer");
  static const pxr::TfToken kEnableThinFilm("inputs:enable_thin_film");
  static const pxr::TfToken kThinFilmThicknessFromAlbedoAlpha("inputs:thin_film_thickness_from_albedo_alpha");
  static const pxr::TfToken kThinFilmThicknessConstant("inputs:thin_film_thickness_constant");
  static const pxr::TfToken kDisplaceIn("inputs:displace_in");

  // Alpha State Overrides
  // Todo: Likely remove these some day in favor of splitting the Opaque material into
  // a legacy material which inherits alpha state information from the drawcall and an opaque material
  // which always controls how its alpha state information is manually (which is what this flag allows).
  static const pxr::TfToken kUseLegacyAlphaState("inputs:use_legacy_alpha_state");
  static const pxr::TfToken kBlendEnabled("inputs:blend_enabled");
  static const pxr::TfToken kBlendType("inputs:blend_type");
  static const pxr::TfToken kInvertedBlend("inputs:inverted_blend");
  static const pxr::TfToken kAlphaTestType("inputs:alpha_test_type");
  static const pxr::TfToken kAlphaReferenceValue("inputs:alpha_test_reference_value");

  // Sprite Sheet attributes
  static const pxr::TfToken kSpriteSheetRowsToken("inputs:sprite_sheet_rows");
  static const pxr::TfToken kSpriteSheetColsToken("inputs:sprite_sheet_cols");
  static const pxr::TfToken kSpriteSheetFPSToken("inputs:sprite_sheet_fps");
  // Portal specific
  static const pxr::TfToken kRayPortalIndexToken("inputs:portal_index");
  static const pxr::TfToken kSpriteRotationSpeedToken("inputs:rotation_speed"); // Radians per second

  // TODO (TREX-1260) Remove legacy Translucent->RayPortal path.
  static const pxr::TfToken kLegacySpriteSheetRowsToken("spriteSheetRows");
  static const pxr::TfToken kLegacySpriteSheetColsToken("spriteSheetCols");
  static const pxr::TfToken kLegacySpriteSheetFPSToken("spriteSheetFPS");
  static const pxr::TfToken kLegacyRayPortalIndexToken("rayPortalIndex");
  static const pxr::TfToken kLegacySpriteRotationSpeedToken("rotationSpeed"); 

  pxr::UsdPrim shader = matPrim.GetChild(kShaderToken);
  if (!shader.IsValid() || !shader.IsA<pxr::UsdShadeShader>()) {
    auto children = matPrim.GetFilteredChildren(pxr::UsdPrimIsActive);
    for (auto child : children) {
      if (child.IsA<pxr::UsdShadeShader>()) {
        shader = child;
      }
    }
  }

  if (!shader.IsValid()) {
    return nullptr;
  }

  XXH64_hash_t materialHash = getMaterialHash(matPrim, shader);
  if (materialHash == 0) {
    return nullptr;
  }

  // Check if the material has already been processed
  MaterialData* materialData;
  if (m_owner.m_replacements->getObject(materialHash, materialData)) {
    return materialData;
  }


  int spriteSheetRows = RtxOptions::Get()->getSharedMaterialDefaults().SpriteSheetRows;
  int spriteSheetCols = RtxOptions::Get()->getSharedMaterialDefaults().SpriteSheetCols;
  int spriteSheetFPS = RtxOptions::Get()->getSharedMaterialDefaults().SpriteSheetFPS;
  bool enableEmission = RtxOptions::Get()->getSharedMaterialDefaults().EnableEmissive;
  float emissiveIntensity = RtxOptions::Get()->getSharedMaterialDefaults().EmissiveIntensity;

  shader.GetAttribute(kEnableEmission).Get(&enableEmission);
  shader.GetAttribute(kEmissiveIntensity).Get(&emissiveIntensity);

  if (shader.HasAttribute(kSpriteSheetFPSToken)) {
    shader.GetAttribute(kSpriteSheetRowsToken).Get(&spriteSheetRows);
    shader.GetAttribute(kSpriteSheetColsToken).Get(&spriteSheetCols);
    shader.GetAttribute(kSpriteSheetFPSToken).Get(&spriteSheetFPS);
  } else if (shader.HasAttribute(kLegacySpriteSheetFPSToken)) {
    // TODO (TREX-1260) Remove legacy Translucent->RayPortal path.
    uint legacySpriteSheetRows = spriteSheetRows;
    uint legacySpriteSheetCols = spriteSheetCols;
    uint legacySpriteSheetFPS = spriteSheetFPS;
    shader.GetAttribute(kLegacySpriteSheetRowsToken).Get(&legacySpriteSheetRows);
    shader.GetAttribute(kLegacySpriteSheetColsToken).Get(&legacySpriteSheetCols);
    shader.GetAttribute(kLegacySpriteSheetFPSToken).Get(&legacySpriteSheetFPS);
    spriteSheetRows = legacySpriteSheetRows;
    spriteSheetCols = legacySpriteSheetCols;
    spriteSheetFPS = legacySpriteSheetFPS;
  }

  bool shouldIgnore = false;
  if (shader.HasAttribute(kIgnore)) {
    shader.GetAttribute(kIgnore).Get(&shouldIgnore);
  }

  // Todo: Only Opaque materials are currently handled, in the future a Translucent path should also exist
  RtSurfaceMaterialType materialType = RtSurfaceMaterialType::Opaque;
  static const pxr::TfToken sourceAsset("info:mdl:sourceAsset");
  pxr::UsdAttribute sourceAssetAttr = shader.GetAttribute(sourceAsset);
  if (sourceAssetAttr.HasValue()) {
    static pxr::SdfAssetPath assetPath;
    sourceAssetAttr.Get(&assetPath);
    std::string assetPathStr = assetPath.GetAssetPath();
    if (assetPathStr.find("AperturePBR_Portal.mdl") != std::string::npos) {
      materialType = RtSurfaceMaterialType::RayPortal;
    } else if (assetPathStr.find("AperturePBR_Translucent.mdl") != std::string::npos) {
      if (shader.HasAttribute(kLegacyRayPortalIndexToken)) {
        // TODO (TREX-1260) Remove legacy Translucent->RayPortal path.
        materialType = RtSurfaceMaterialType::RayPortal;
      } else {
        materialType = RtSurfaceMaterialType::Translucent;
      }
    }
  }

  if (materialType == RtSurfaceMaterialType::Translucent) {
    float refractiveIndex = RtxOptions::Get()->getTranslucentMaterialDefaults().RefractiveIndex;
    Vector3 transmittanceColor = RtxOptions::Get()->getTranslucentMaterialDefaults().TransmittanceColor;
    float transmittanceMeasureDistance = RtxOptions::Get()->getTranslucentMaterialDefaults().TransmittanceMeasurementDistance;
    Vector3 emissiveColorConstant = RtxOptions::Get()->getTranslucentMaterialDefaults().EmissiveColorConstant;
    bool isThinWalled = RtxOptions::Get()->getTranslucentMaterialDefaults().ThinWalled;
    float thinWallThickness = RtxOptions::Get()->getTranslucentMaterialDefaults().ThinWallThickness;
    bool useDiffuseLayer = RtxOptions::Get()->getTranslucentMaterialDefaults().UseDiffuseLayer;

    shader.GetAttribute(kIORConstant).Get(&refractiveIndex);

    getVector3(shader, kTransmittanceConstant, transmittanceColor);

    shader.GetAttribute(kTransmittanceDistanceConstant).Get(&transmittanceMeasureDistance);

    getVector3(shader, kEmissiveColorConstant, emissiveColorConstant);

    shader.GetAttribute(kIsThinWalled).Get(&isThinWalled);
    shader.GetAttribute(kThinWallThickness).Get(&thinWallThickness);
    shader.GetAttribute(kUseDiffuseLayer).Get(&useDiffuseLayer);

    const TextureRef normalTexture(getTexture(args, shader, kNormalTextureToken));
    const TextureRef transmittanceTexture(getTexture(args, shader, kTransmittanceTexture));
    // Note: Only set if in use to avoid sampling from this texture if emission is disabled.
    TextureRef emissiveColorTexture {};

    if (enableEmission) {
      emissiveColorTexture = TextureRef{ getTexture(args, shader, kEmissiveMaskTextureToken) };
    }

    const TranslucentMaterialData translucentMaterialData{
      normalTexture, transmittanceTexture, emissiveColorTexture,
      refractiveIndex,
      transmittanceColor, transmittanceMeasureDistance,
      enableEmission, emissiveIntensity, emissiveColorConstant,
      static_cast<uint8_t>(spriteSheetRows),
      static_cast<uint8_t>(spriteSheetCols),
      static_cast<uint8_t>(spriteSheetFPS),
      isThinWalled, thinWallThickness, useDiffuseLayer
    };

    return &m_owner.m_replacements->storeObject(materialHash, MaterialData(translucentMaterialData, shouldIgnore));
  } else if (materialType == RtSurfaceMaterialType::Opaque) {
    float anisotropy = RtxOptions::Get()->getOpaqueMaterialDefaults().Anisotropy;
    Vector4 albedoOpacityConstant = RtxOptions::Get()->getOpaqueMaterialDefaults().AlbedoOpacityConstant;
    float roughnessConstant = RtxOptions::Get()->getOpaqueMaterialDefaults().RoughnessConstant;
    float metallicConstant = RtxOptions::Get()->getOpaqueMaterialDefaults().MetallicConstant;
    Vector3 emissiveColorConstant = RtxOptions::Get()->getOpaqueMaterialDefaults().EmissiveColorConstant;
    float thinFilmThicknessConstant = RtxOptions::Get()->getOpaqueMaterialDefaults().ThinFilmThicknessConstant;
    bool alphaIsThinFilmThickness = RtxOptions::Get()->getOpaqueMaterialDefaults().AlphaIsThinFilmThickness;
    bool useLegacyAlphaState = RtxOptions::Get()->getOpaqueMaterialDefaults().UseLegacyAlphaState;
    bool blendEnabled = RtxOptions::Get()->getOpaqueMaterialDefaults().BlendEnabled;
    BlendType blendType = RtxOptions::Get()->getOpaqueMaterialDefaults().DefaultBlendType;
    bool invertedBlend = RtxOptions::Get()->getOpaqueMaterialDefaults().InvertedBlend;
    AlphaTestType alphaTestType = RtxOptions::Get()->getOpaqueMaterialDefaults().DefaultAlphaTestType;
    uint8_t alphaReferenceValue = RtxOptions::Get()->getOpaqueMaterialDefaults().AlphaReferenceValue;
    float displaceIn = RtxOptions::Get()->getOpaqueMaterialDefaults().DisplaceIn;

    shader.GetAttribute(kOpacityConstant).Get(&albedoOpacityConstant.a);

    shader.GetAttribute(kAnisotropy).Get(&anisotropy);

    getVector3(shader, kAlbedoConstant, albedoOpacityConstant.xyz());

    shader.GetAttribute(kRoughnessConstant).Get(&roughnessConstant);
    shader.GetAttribute(kMetallicConstant).Get(&metallicConstant);
    shader.GetAttribute(kDisplaceIn).Get(&displaceIn);

    getVector3(shader, kEmissiveColorConstant, emissiveColorConstant);

    const TextureRef albedoTexture(getTexture(args, shader, kAlbedoTextureToken));
    const TextureRef normalTexture(getTexture(args, shader, kNormalTextureToken));
    const TextureRef tangentTexture(getTexture(args, shader, kTangentTextureToken));
    const TextureRef heightTexture(getTexture(args, shader, kHeightTextureToken));
    const TextureRef roughnessTexture(getTexture(args, shader, kRoughnessTextureToken));
    const TextureRef metallicTexture(getTexture(args, shader, kMetallicTextureToken));
    // Note: Only set if in use to avoid sampling from this texture if emission is disabled.
    TextureRef emissiveColorTexture{};

    if (enableEmission) {
      emissiveColorTexture = TextureRef{ getTexture(args, shader, kEmissiveMaskTextureToken) };
    }

    bool thinFilmEnable = false;
    shader.GetAttribute(kEnableThinFilm).Get(&thinFilmEnable);

    if (thinFilmEnable) {
      shader.GetAttribute(kThinFilmThicknessFromAlbedoAlpha).Get(&alphaIsThinFilmThickness);
      if (!alphaIsThinFilmThickness) {
        shader.GetAttribute(kThinFilmThicknessConstant).Get(&thinFilmThicknessConstant);
      }
    }

    shader.GetAttribute(kUseLegacyAlphaState).Get(&useLegacyAlphaState);

    if (!useLegacyAlphaState) {
      shader.GetAttribute(kBlendEnabled).Get(&blendEnabled);

      if (blendEnabled) {
        int rawBlendType;
        shader.GetAttribute(kBlendType).Get(&rawBlendType);

        blendType = static_cast<BlendType>(rawBlendType);

        shader.GetAttribute(kInvertedBlend).Get(&invertedBlend);
      }

      int rawAlphaTestType;
      shader.GetAttribute(kAlphaTestType).Get(&rawAlphaTestType);

      alphaTestType = static_cast<AlphaTestType>(rawAlphaTestType);

      float normalizedAlphaReferenceValue;
      shader.GetAttribute(kAlphaReferenceValue).Get(&normalizedAlphaReferenceValue);

      // Note: Convert 0-1 floating point alpha reference value in MDL to 0-255 uint8 used for rendering.
      alphaReferenceValue = static_cast<uint8_t>(std::numeric_limits<uint8_t>::max() * normalizedAlphaReferenceValue);
    }

    const OpaqueMaterialData opaqueMaterialData{
      albedoTexture, normalTexture,
      tangentTexture, heightTexture, roughnessTexture,
      metallicTexture, emissiveColorTexture,
      anisotropy, emissiveIntensity,
      albedoOpacityConstant,
      roughnessConstant, metallicConstant,
      emissiveColorConstant, enableEmission,
      static_cast<uint8_t>(spriteSheetRows),
      static_cast<uint8_t>(spriteSheetCols),
      static_cast<uint8_t>(spriteSheetFPS),
      thinFilmEnable,
      alphaIsThinFilmThickness,
      thinFilmThicknessConstant,
      useLegacyAlphaState, blendEnabled, blendType, invertedBlend,
      alphaTestType, alphaReferenceValue, displaceIn
    };

    return &m_owner.m_replacements->storeObject(materialHash, MaterialData(opaqueMaterialData, shouldIgnore));
  } else if (materialType == RtSurfaceMaterialType::RayPortal) {
    TextureRef albedoTexture;
    
    int rayPortalIndex = RtxOptions::Get()->getRayPortalMaterialDefaults().RayPortalIndex;
    float rotationSpeed = RtxOptions::Get()->getRayPortalMaterialDefaults().RotationSpeed;

    // we set the forcePreload flag in the calls to getTexture below to make sure the portal textures
    // are loaded at init time, otherwise we get a hitch the first time a portal is placed
    //
    // in the future, we should try to get this info directly from the toolkit, to allow artists to tag textures
    // for preloading instead of relying on material hash lists
    if (shader.HasAttribute(kRayPortalIndexToken)){
      shader.GetAttribute(kRayPortalIndexToken).Get(&rayPortalIndex);
      shader.GetAttribute(kSpriteRotationSpeedToken).Get(&rotationSpeed);
      albedoTexture = TextureRef(getTexture(args, shader, kEmissiveMaskTextureToken, true));
    } else if (shader.HasAttribute(kLegacyRayPortalIndexToken)) {
      // TODO (TREX-1260) Remove legacy Translucent->RayPortal path.
      uint legacyIndex = rayPortalIndex;
      shader.GetAttribute(kLegacyRayPortalIndexToken).Get(&legacyIndex);
      rayPortalIndex = legacyIndex;
      shader.GetAttribute(kLegacySpriteRotationSpeedToken).Get(&rotationSpeed);
      albedoTexture = TextureRef(getTexture(args, shader, kAlbedoTextureToken, true));
    }

    const RayPortalMaterialData rayPortalMaterialData{
      albedoTexture, albedoTexture,
      static_cast<uint8_t>(rayPortalIndex), static_cast<uint8_t>(spriteSheetRows),
      static_cast<uint8_t>(spriteSheetCols), static_cast<uint8_t>(spriteSheetFPS),
      rotationSpeed, enableEmission, emissiveIntensity
    };

    return &m_owner.m_replacements->storeObject(materialHash, MaterialData(rayPortalMaterialData));
  }

  return nullptr;
}

MaterialData* UsdMod::Impl::processMaterialUser(Args& args, const pxr::UsdPrim& prim) {
  auto bindAPI = pxr::UsdShadeMaterialBindingAPI(prim);
  auto boundMaterial = bindAPI.ComputeBoundMaterial();
  if (boundMaterial) {
    return processMaterial(args, boundMaterial.GetPrim());
  }
  return nullptr;
}

void UsdMod::Impl::processPrim(Args& args, pxr::UsdPrim& prim) {
  ScopedCpuProfileZone();

  const XXH64_hash_t usdOriginHash = getStrongestOpinionatedPathHash(prim);


  MeshReplacement* pTemp;
  if (!m_owner.m_replacements->getObject(usdOriginHash, pTemp)) {
    // First time seeing this mesh, then process it.
    if (!processMesh(prim, args)) {
      return;
    }
  }

  MaterialData* materialData = processMaterialUser(args, prim);

  pxr::GfMatrix4f localToRoot = pxr::GfMatrix4f(args.xformCache.GetLocalToWorldTransform(prim));

  if (RtxOptions::Get()->isLHS()) {
    static pxr::GfMatrix4f XYflip(pxr::GfVec4f(1.0, 1.0, -1.0, 1.0));
    // Change of Basis transform
    // X' = P * X * P-1
    localToRoot = XYflip * localToRoot * XYflip;
  }

  const auto& replacementToObjectAsArray = reinterpret_cast<const float(&)[4][4]>(localToRoot);
  const Matrix4 replacementToObject(replacementToObjectAsArray);

  std::vector<pxr::UsdGeomSubset> geomSubsets;
  auto children = prim.GetFilteredChildren(pxr::UsdPrimIsActive);
  for (auto child : children) {
    if (child.IsA<pxr::UsdGeomSubset>()) {
      geomSubsets.emplace_back(child);
    }
  }

  Categorizer categoryFlags = processCategoryFlags(prim);

  if (geomSubsets.empty()) {
    MeshReplacement* pGeometryData;
    if (m_owner.m_replacements->getObject(usdOriginHash, pGeometryData)) {
      AssetReplacement newReplacementMesh(pGeometryData, materialData, categoryFlags, replacementToObject);
      args.meshes.push_back(newReplacementMesh);
    }
  } else {
    for (auto subset : geomSubsets) {
      const XXH64_hash_t usdChildOriginHash = getStrongestOpinionatedPathHash(subset.GetPrim());
      MeshReplacement* childGeometryData;
      if (m_owner.m_replacements->getObject(usdChildOriginHash, childGeometryData)) {
        AssetReplacement newReplacementMesh(childGeometryData, materialData, categoryFlags, replacementToObject);
        MaterialData* mat = processMaterialUser(args, subset.GetPrim());
        if (mat) {
          newReplacementMesh.materialData = mat;
        }
        args.meshes.push_back(newReplacementMesh);
      }
    }
  }
}

bool IsLight(const pxr::UsdPrim& lightPrim) {
  return lightPrim.IsA<pxr::UsdLuxSphereLight>()
      || lightPrim.IsA<pxr::UsdLuxRectLight>()
      || lightPrim.IsA<pxr::UsdLuxDiskLight>()
      || lightPrim.IsA<pxr::UsdLuxCylinderLight>()
      || lightPrim.IsA<pxr::UsdLuxDistantLight>();
}

void UsdMod::Impl::processLight(Args& args, const pxr::UsdPrim& lightPrim) {
  static const pxr::TfToken kRadiusToken("radius");
  static const pxr::TfToken kWidthToken("width");
  static const pxr::TfToken kHeightToken("height");
  static const pxr::TfToken kLengthToken("length");
  static const pxr::TfToken kAngleToken("angle");
  static const pxr::TfToken kInputsRadiusToken("inputs:radius");
  static const pxr::TfToken kInputsWidthToken("inputs:width");
  static const pxr::TfToken kInputsHeightToken("inputs:height");
  static const pxr::TfToken kInputsLengthToken("inputs:length");
  static const pxr::TfToken kInputsAngleToken("inputs:angle");
  static constexpr float degreesToRadians = float(M_PI / 180.0);
  RtLight genericLight;
  if (args.rootPrim.IsA<pxr::UsdGeomMesh>() && lightPrim.IsA<pxr::UsdLuxDistantLight>()) {
    Logger::err(str::format("A DistantLight detect under ", args.rootPrim.GetName(),
        " will be ignored.  DistantLights are only supported as part of light replacements, not mesh replacements."));
  }
  
  pxr::GfMatrix4f localToRoot;
  // Need to preserve the root's transform if it is a light, but ignore it if it's a mesh.
  // Lights being replaced are instances that need to exist in the same place as the drawcall they're replacing.
  // Meshes being replaced are assets that may have multiple instances, so any children need to be offset from the
  // asset root, instead of the world root.
  if (IsLight(args.rootPrim)) {
    localToRoot = pxr::GfMatrix4f(args.xformCache.GetLocalToWorldTransform(lightPrim));
  } else {
    bool resetXformStack; // unused
    localToRoot = pxr::GfMatrix4f(args.xformCache.ComputeRelativeTransform(lightPrim, args.rootPrim, &resetXformStack));
  }

  pxr::GfVec3f xVecUsd = localToRoot.TransformDir(pxr::GfVec3f(1.0f, 0.0f, 0.0f));
  pxr::GfVec3f yVecUsd = localToRoot.TransformDir(pxr::GfVec3f(0.0f, 1.0f, 0.0f));
  pxr::GfVec3f zVecUsd = localToRoot.TransformDir(pxr::GfVec3f(0.0f, 0.0f, 1.0f));
  
  float xScale = xVecUsd.Normalize();
  float yScale = yVecUsd.Normalize();
  float zScale = zVecUsd.Normalize();

  const Vector3 position(localToRoot.ExtractTranslation().data());
  const Vector3 xAxis(xVecUsd.GetArray());
  const Vector3 yAxis(yVecUsd.GetArray());
  const Vector3 zAxis(zVecUsd.GetArray());
  
  // Calculate light color.  Based on `getFinalLightColor` in Kit's LightContext.cpp.
  static const pxr::TfToken kEnableColorTemperatureToken("enableColorTemperature");
  static const pxr::TfToken kColorToken("color");
  static const pxr::TfToken kColorTemperatureToken("colorTemperature");
  static const pxr::TfToken kIntensityToken("intensity");
  static const pxr::TfToken kExposureToken("exposure");
  static const pxr::TfToken kInputsEnableColorTemperatureToken("inputs:enableColorTemperature");
  static const pxr::TfToken kInputsColorToken("inputs:color");
  static const pxr::TfToken kInputsColorTemperatureToken("inputs:colorTemperature");
  static const pxr::TfToken kInputsIntensityToken("inputs:intensity");
  static const pxr::TfToken kInputsExposureToken("inputs:exposure");
  Vector3 radiance(1.f);
  Vector3 temperature(1.f);
  float exposure = 0.0f;
  float intensity = 0.0f;

  getVector3(getLightAttribute(lightPrim, kColorToken, kInputsColorToken), radiance);
  bool enableColorTemperature = false;
  getLightAttribute(lightPrim, kEnableColorTemperatureToken, kInputsEnableColorTemperatureToken).Get(&enableColorTemperature);
  if (enableColorTemperature) {
    pxr::UsdAttribute colorTempAttr = getLightAttribute(lightPrim, kColorTemperatureToken, kInputsColorTemperatureToken);
    if (colorTempAttr.HasValue()) {
      float temp = 6500.f;
      colorTempAttr.Get(&temp);
      pxr::GfVec3f vec = pxr::UsdLuxBlackbodyTemperatureAsRgb(temp);
      temperature = Vector3(vec.data());
    }
  }
  getLightAttribute(lightPrim, kExposureToken, kInputsExposureToken).Get(&exposure);
  getLightAttribute(lightPrim, kIntensityToken, kInputsIntensityToken).Get(&intensity);
  
  radiance = radiance * intensity * pow(2, exposure) * temperature;

  // Per Light type properties.
  if (lightPrim.IsA<pxr::UsdLuxSphereLight>()) {
    float radius;
    getLightAttribute(lightPrim, kRadiusToken, kInputsRadiusToken).Get(&radius);
    RtLightShaping shaping = getLightShaping(lightPrim, -zAxis);
    genericLight = RtLight(RtSphereLight(position, radiance, radius, shaping));
  } else if (lightPrim.IsA<pxr::UsdLuxRectLight>()) {
    float width, height = 0.0f;
    getLightAttribute(lightPrim, kWidthToken, kInputsWidthToken).Get(&width);
    getLightAttribute(lightPrim, kHeightToken, kInputsHeightToken).Get(&height);
    Vector2 dimensions(width * xScale, height * yScale);
    RtLightShaping shaping = getLightShaping(lightPrim, zAxis);
    genericLight = RtLight(RtRectLight(position, dimensions, xAxis, yAxis, radiance, shaping));
  } else if (lightPrim.IsA<pxr::UsdLuxDiskLight>()) {
    float radius;
    getLightAttribute(lightPrim, kRadiusToken, kInputsRadiusToken).Get(&radius);
    Vector2 halfDimensions(radius * xScale, radius * yScale);
    RtLightShaping shaping = getLightShaping(lightPrim, zAxis);
    genericLight = RtLight(RtDiskLight(position, halfDimensions, xAxis, yAxis, radiance, shaping));
  } else if (lightPrim.IsA<pxr::UsdLuxCylinderLight>()) {
    float radius;
    getLightAttribute(lightPrim, kRadiusToken, kInputsRadiusToken).Get(&radius);
    float axisLength;
    getLightAttribute(lightPrim, kLengthToken, kInputsLengthToken).Get(&axisLength);
    genericLight = RtLight(RtCylinderLight(position, radius, xAxis, axisLength * xScale, radiance));
  } else if (lightPrim.IsA<pxr::UsdLuxDistantLight>()) {
    float halfAngle;
    getLightAttribute(lightPrim, kAngleToken, kInputsAngleToken).Get(&halfAngle);
    halfAngle = halfAngle * degreesToRadians / 2.0f;
    genericLight = RtLight(RtDistantLight(zAxis, halfAngle, radiance));
  } else {
    return;
  }
  
  args.meshes.emplace_back(genericLight);
}

void UsdMod::Impl::processReplacement(Args& args) {
  ScopedCpuProfileZone();
  static const pxr::TfToken kPreserveOriginalToken("preserveOriginalDrawCall");

  if (args.rootPrim.IsA<pxr::UsdGeomMesh>()) {
    processPrim(args, args.rootPrim);
  } else if (IsLight(args.rootPrim)) {
    processLight(args, args.rootPrim);
  }
  auto descendents = args.rootPrim.GetFilteredDescendants(pxr::UsdPrimIsActive);
  for (auto desc : descendents) {
    if (desc.IsA<pxr::UsdGeomMesh>()) {
      processPrim(args, desc);
    } else if (IsLight(desc)) {
      processLight(args, desc);
    }
  }
  
  if (!args.meshes.empty() && args.rootPrim.HasAttribute(kPreserveOriginalToken)) {
    int preserve = 0;
    args.rootPrim.GetAttribute(kPreserveOriginalToken).Get(&preserve);
    args.meshes[0].includeOriginal = preserve != 0;

    // Read category flags if we include the original
    if (args.meshes[0].includeOriginal) {
      args.meshes[0].categories = processCategoryFlags(args.rootPrim);
    }
  }
}

void UsdMod::Impl::load(const Rc<DxvkContext>& context) {
  ScopedCpuProfileZone();
  if (m_owner.state() == State::Unloaded) {
    processUSD(context);

    m_usdChangeWatchdog.start();
  }
}

void UsdMod::Impl::unload() {
  if (m_owner.state() == State::Loaded) {
    m_usdChangeWatchdog.stop();

    m_owner.m_replacements->clear();
    AssetDataManager::get().clearSearchPaths();

    m_owner.setState(State::Unloaded);
  }
}

bool UsdMod::Impl::haveFilesChanged() {
  if (m_openedFilePath.empty())
    return false;

  fs::file_time_type newModTime;
  if (m_owner.state() == State::Loaded) {
    newModTime = fs::last_write_time(fs::path(m_openedFilePath));
  } else {
    bool fileFound = false;
    const auto replacementsUsdPath = fs::path(m_openedFilePath);
    fileFound = fs::exists(replacementsUsdPath);
    if (fs::exists(replacementsUsdPath)) {
      newModTime = fs::last_write_time(replacementsUsdPath);
    } else {
      m_owner.setState(State::Unloaded);
      return false;
    }
  }
  return (newModTime > m_fileModificationTime);
}

bool UsdMod::Impl::checkForChanges(const Rc<DxvkContext>& context) {
  if (m_usdChangeWatchdog.hasSignaled()) {
    unload();
    load(context);
    return true;
  }

  return false;
}

void UsdMod::Impl::processUSD(const Rc<DxvkContext>& context) {
  ScopedCpuProfileZone();
  std::string replacementsUsdPath(m_owner.m_filePath.string());

  m_owner.setState(State::Loading);

  pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(replacementsUsdPath, pxr::UsdStage::LoadAll);

  if (!stage) {
    Logger::err(str::format("USD mod file failed parsing: ", std::filesystem::weakly_canonical(replacementsUsdPath).string()));
    m_openedFilePath.clear();
    m_fileModificationTime = fs::file_time_type();
    m_owner.setState(State::Unloaded);
    return;
  }

  std::filesystem::path modBaseDirectory = std::filesystem::path(replacementsUsdPath).remove_filename();
  m_openedFilePath = replacementsUsdPath;

  // Iterate sublayers in the strength order, resolve the base paths and
  // populate asset manager search paths.
  auto sublayers = stage->GetRootLayer()->GetSubLayerPaths();
  for (size_t i = 0, s = sublayers.size(); i < s; i++) {
    const std::string& identifier = sublayers[i];
    auto layerBasePath = std::filesystem::path(identifier).remove_filename();
    auto fullLayerBasePath = modBaseDirectory / layerBasePath;
    AssetDataManager::get().addSearchPath(i, fullLayerBasePath);
  }

  // Add stage's base path last.
  AssetDataManager::get().addSearchPath(sublayers.size(), modBaseDirectory);

  m_fileModificationTime = fs::last_write_time(fs::path(m_openedFilePath));
  pxr::UsdGeomXformCache xformCache;

  pxr::VtDictionary layerData = stage->GetRootLayer()->GetCustomLayerData();
  if (layerData.empty()) {
    m_owner.m_status = "Layer Data Missing";
  } else {
    const PXR_NS::VtValue* vtExportStatus = layerData.GetValueAtPath(kStatusKey);
    if (vtExportStatus && !vtExportStatus->IsEmpty()) {
      m_owner.m_status = vtExportStatus->Get<std::string>();
    } else {
      m_owner.m_status = "Status Missing";
    }
  }

  fast_unordered_cache<uint32_t> variantCounts;
  pxr::UsdPrim meshes = stage->GetPrimAtPath(pxr::SdfPath("/RootNode/meshes"));
  if (meshes.IsValid()) {
    auto children = meshes.GetFilteredChildren(pxr::UsdPrimIsActive);
    for (pxr::UsdPrim child : children) {
      XXH64_hash_t hash = getModelHash(child);
      if (hash != 0) {
        std::vector<AssetReplacement> replacementVec;
        
        Args args = {context, xformCache, child, replacementVec};

        processReplacement(args);

        variantCounts[hash]++;

        m_owner.m_replacements->set<AssetReplacement::eMesh>(hash, std::move(replacementVec));
      }
    }
  }

  // TODO: enter "secrets" section of USD as exported by Kit app
  TEMP_parseSecretReplacementVariants(variantCounts);
  for (auto& [hash, secretReplacements] : m_owner.m_replacements->secretReplacements()) {
    for (auto& secretReplacement : secretReplacements) {
      const std::string variantStage(modBaseDirectory.string() + secretReplacement.replacementPath);
      double dummy;
      if (!pxr::ArchGetModificationTime(variantStage.c_str(),&dummy)) {
        Logger::warn(
          std::string("[SecretReplacement] Could not find stage: ") + variantStage);
        continue;
      }
      auto pStage = pxr::UsdStage::Open(variantStage, pxr::UsdStage::LoadAll);
      if (!pStage) {
        Logger::err(
          std::string("[SecretReplacement] Failed to open stage: ") + variantStage);
        continue;
      }
      auto rootPrim = pStage->GetDefaultPrim();
      auto variantHash = hash + secretReplacement.variantId;
      std::vector<AssetReplacement> replacementVec;

      Args args = {context, xformCache, rootPrim, replacementVec};

      processReplacement(args);

      m_owner.m_replacements->set<AssetReplacement::eMesh>(variantHash, std::move(replacementVec));
    }
  }

  pxr::UsdPrim lights = stage->GetPrimAtPath(pxr::SdfPath("/RootNode/lights"));
  if (lights.IsValid()) {
    auto children = lights.GetFilteredChildren(pxr::UsdPrimIsActive);
    for (pxr::UsdPrim child : children) {
      XXH64_hash_t hash = getLightHash(child);
      if (hash != 0) {
        std::vector<AssetReplacement> replacementVec;
        Args args = {context, xformCache, child, replacementVec};

        processReplacement(args);

        m_owner.m_replacements->set<AssetReplacement::eLight>(hash, std::move(replacementVec));
      }
    }
  }

  pxr::UsdPrim materialRoot = stage->GetPrimAtPath(pxr::SdfPath("/RootNode/Looks"));
  if (materialRoot.IsValid()) {
    auto children = materialRoot.GetFilteredChildren(pxr::UsdPrimIsActive);
    std::vector<AssetReplacement> placeholder;

    Args args = {context, xformCache, materialRoot, placeholder};

    for (pxr::UsdPrim materialPrim : children) {
      processMaterial(args, materialPrim);
    }
  }

  // flush entire cache, kinda a sledgehammer
  context->emitMemoryBarrier(0,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

  m_owner.setState(State::Loaded);
}

void UsdMod::Impl::TEMP_parseSecretReplacementVariants(const fast_unordered_cache<uint32_t>& variantCounts) {
  auto lookupCount = [&variantCounts](XXH64_hash_t hash) -> auto {
    // NOTE: If there's no default replacement make sure secret variants are not default.
    return variantCounts.count(hash) ? variantCounts.at(hash) : 1u;
  };

  static constexpr XXH64_hash_t kStorageCubeHash = 0xc728cfe75526c741;
  uint32_t numVariants = lookupCount(kStorageCubeHash);
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","Ice","",
    0x60ead40e2269b3c5,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_CompanionCube_Ice.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","Lens","",
    0xa8e871f4ebc52eab,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_CompanionCube_Lens.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","Camera","",
    0xd150bdeff3f0299a,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_CompanionCubeCamera_A01_01.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","Digital Skull","",
    0xb26578451f75c11a,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_CompanionCubeDigital_A02_01.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","Iso-Wheatly","",
    0xc270f63a956c0c71,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_CompanionCubeIsogrid_A01_01.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","Iso-Voyager","",
    0xaaaf0cbd8c8204cd,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_CompanionCubeIsogrid_A02_01.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","Iso-Black-Mesa","",
    0x2f9fe4ce23a83bc2,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_CompanionCubeIsogrid_A03_01.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kStorageCubeHash, SecretReplacement{
    "Storage Cubes","RTX","",
    0xe361f386c03400f3,
    kStorageCubeHash,
    "./SubUSDs/SM_Prop_RTX_CompanionCube_A1_01.usd",
    true,
    true,
    numVariants++});

  static constexpr XXH64_hash_t kCompanionCubeHash = 0x6ef165bb7e0b8512;
  numVariants = lookupCount(kCompanionCubeHash);
  m_owner.m_replacements->storeObject(kCompanionCubeHash, SecretReplacement{
    "Companion Cubes","Pillow","",
    0xc901411d90916a58,
    kCompanionCubeHash,
    "./SubUSDs/SM_Prop_CompanionCube_Pillow_A.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kCompanionCubeHash, SecretReplacement{
    "Companion Cubes","Ceramic","",
    0x3495c5b9d210daa1,
    kCompanionCubeHash,
    "./SubUSDs/SM_Prop_CompanionCube_Ceramic.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kCompanionCubeHash, SecretReplacement{
    "Companion Cubes","Wood","",
    0x5e50cb7c64375acc,
    kCompanionCubeHash,
    "./SubUSDs/SM_Prop_CompanionCube_Wood.usd",
    true,
    true,
    numVariants++});
  m_owner.m_replacements->storeObject(kCompanionCubeHash, SecretReplacement{
    "Companion Cubes","Digital","",
    0xf2bda31c09fc42f6,
    kCompanionCubeHash,
    "./SubUSDs/SM_Prop_CompanionCubeDigital_A01_01.usd",
    true,
    true,
    numVariants++});
}


Categorizer UsdMod::Impl::processCategoryFlags(const pxr::UsdPrim& prim) {
  Categorizer categoryFlags;
  for (uint32_t i = 0; i < (uint32_t) InstanceCategories::Count; i++) {
    const char* categoryName = getInstanceCategorySubKey((InstanceCategories) i);
    pxr::TfToken token = pxr::TfToken(categoryName);
    if (!prim.HasAttribute(token)) {
      continue;
    }

    pxr::VtValue value;
    if (!prim.GetAttribute(token).Get(&value)) {
      continue;
    }

    categoryFlags.categoryExists.set((InstanceCategories) i);
    if (value.Get<bool>()) {
      categoryFlags.categoryFlags.set((InstanceCategories) i);
    }
  }

  return categoryFlags;
}

bool UsdMod::Impl::processMesh(const pxr::UsdPrim& prim, Args& args) {
  MeshReplacement replacement;
  RasterGeometry& geometryData = replacement.data;

  std::unique_ptr<lss::UsdMeshImporter> processedMesh;

  try {
    processedMesh = std::make_unique<lss::UsdMeshImporter>(prim);
  }
  catch (DxvkError e) {
    Logger::err(e.message());
    return false;
  }

  geometryData.vertexCount = processedMesh->GetNumVertices();

  if (processedMesh->GetNumVertices() == 0) {
    throw DxvkError(str::format("Warning: No vertices on this mesh after processing, id=.", prim.GetName()));
  }

  const size_t vertexDataSize = processedMesh->GetNumVertices() * processedMesh->GetVertexStride();

  // Allocate the instance buffer and copy its contents from host to device memory
  DxvkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
  info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
  info.size = dxvk::align(vertexDataSize, CACHE_LINE_SIZE);

  // Buffer contains:
  // |---POSITIONS---|---NORMALS---|---UVS---| ... (VERTEX DATA INTERLEAVED)
  Rc<DxvkBuffer> vertexBuffer = args.context->getDevice()->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, DxvkMemoryStats::Category::RTXBuffer);
  const DxvkBufferSlice& vertexSlice = DxvkBufferSlice(vertexBuffer);
  memcpy(vertexSlice.mapPtr(0), processedMesh->GetVertexData().data(), vertexDataSize);

  for (const auto& element : processedMesh->GetVertexDecl()) {
    switch (element.attribute) {
    case lss::UsdMeshImporter::VertexPositions:
      geometryData.positionBuffer = RasterBuffer(vertexSlice, element.offset, processedMesh->GetVertexStride(), VK_FORMAT_R32G32B32_SFLOAT);
      break;
    case lss::UsdMeshImporter::Normals:
      geometryData.normalBuffer = RasterBuffer(vertexSlice, element.offset, processedMesh->GetVertexStride(), VK_FORMAT_R32G32B32_SFLOAT);
      break;
    case lss::UsdMeshImporter::Texcoords:
      geometryData.texcoordBuffer = RasterBuffer(vertexSlice, element.offset, processedMesh->GetVertexStride(), VK_FORMAT_R32G32_SFLOAT);
      geometryData.hashes[HashComponents::VertexTexcoord] = getNextGeomHash();
      break;
    case lss::UsdMeshImporter::Colors:
      geometryData.color0Buffer = RasterBuffer(vertexSlice, element.offset, processedMesh->GetVertexStride(), VK_FORMAT_R8G8B8A8_UNORM);
      break;
    case lss::UsdMeshImporter::BlendWeights:
      geometryData.blendWeightBuffer = RasterBuffer(vertexSlice, element.offset, processedMesh->GetVertexStride(), VK_FORMAT_R32_SFLOAT);
      // Note: only want to set this when there are actually weights, as it triggers the replacement to be skinned.
      geometryData.numBonesPerVertex = processedMesh->GetNumBonesPerVertex(); // TODO: Implement this in UsdMesh
      break;
    case lss::UsdMeshImporter::BlendIndices:
      geometryData.blendIndicesBuffer = RasterBuffer(vertexSlice, element.offset, processedMesh->GetVertexStride(), VK_FORMAT_R8G8B8A8_USCALED);
      break;
    }
  }

  geometryData.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if (processedMesh->GetDoubleSidedState() != lss::UsdMeshImporter::Inherit) {
    const VkCullModeFlagBits singleSidedCullMode = processedMesh->IsRightHanded() ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
    geometryData.cullMode = processedMesh->GetDoubleSidedState() == lss::UsdMeshImporter::IsDoubleSided ? VK_CULL_MODE_NONE : singleSidedCullMode;
    geometryData.forceCullBit = true; // Overrule the instance face culling rules
  } else {
    // In this case we use the face culling set from the application for this mesh
    geometryData.cullMode = VK_CULL_MODE_NONE;
  }

  geometryData.frontFace = processedMesh->IsRightHanded() ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;

  for (const lss::UsdMeshImporter::SubMesh& submesh : processedMesh->GetSubMeshes()) {
    if (submesh.GetNumIndices() == 0) {
      Logger::err(str::format("Prim: ", submesh.prim.GetPath().GetString(), ", does not have indices, this is currently a requirement."));
      continue;
    }

    XXH64_hash_t usdOriginHash = getStrongestOpinionatedPathHash(submesh.prim);
    MeshReplacement* childGeometryData;
    if (!m_owner.m_replacements->getObject(usdOriginHash, childGeometryData)) {
      MeshReplacement& newReplacement = m_owner.m_replacements->storeObject(usdOriginHash, MeshReplacement(replacement));
      RasterGeometry& newGeomData = newReplacement.data;

      const size_t indexDataSize = submesh.GetNumIndices() * sizeof(uint32_t);
      info.size = dxvk::align(indexDataSize, CACHE_LINE_SIZE);

      // Buffer contains: indices
      Rc<DxvkBuffer> indexBuffer = args.context->getDevice()->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, DxvkMemoryStats::Category::RTXBuffer);
      const DxvkBufferSlice& indexSlice = DxvkBufferSlice(indexBuffer);
      memcpy(indexSlice.mapPtr(0), submesh.indexBuffer.data(), indexDataSize);
      newGeomData.indexBuffer = RasterBuffer(indexSlice, 0, sizeof(uint32_t), VK_INDEX_TYPE_UINT32);
      newGeomData.indexCount = submesh.GetNumIndices();
      // Set these as hashed so that the geometryData acts like it's static.
      newGeomData.hashes[HashComponents::Indices] = newGeomData.hashes[HashComponents::VertexPosition] = getNextGeomHash();
      newGeomData.hashes.precombine();
    }
  }

  return true;
}

UsdMod::UsdMod(const Mod::Path& usdFilePath)
: Mod(usdFilePath) {
  m_impl = std::make_unique<Impl>(*this);
}

UsdMod::~UsdMod() {
}

void UsdMod::load(const Rc<DxvkContext>& context) {
  m_impl->load(context);
}

void UsdMod::unload() {
  m_impl->unload();
}

bool UsdMod::checkForChanges(const Rc<DxvkContext>& context) {
  return m_impl->checkForChanges(context);
}

struct UsdModTypeInfo final : public ModTypeInfo {
  std::unique_ptr<Mod> construct(const Mod::Path& modFilePath) const {
    return std::unique_ptr<UsdMod>(new UsdMod(modFilePath));
  }

  bool isValidMod(const Mod::Path& modFilePath) const {
    const auto ext = modFilePath.extension().string();
    for (auto& usdExt : lss::usdExts) {
      if (ext == usdExt.str) {
        return true;
      }
    }
    return false;
  }
};

const ModTypeInfo& UsdMod::getTypeInfo() {
  static UsdModTypeInfo s_typeInfo;
  return s_typeInfo;
}

} // namespace dxvk
