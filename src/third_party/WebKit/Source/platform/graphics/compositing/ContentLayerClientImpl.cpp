// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/graphics/compositing/ContentLayerClientImpl.h"

#include "cc/paint/paint_op_buffer.h"
#include "platform/geometry/GeometryAsJSON.h"
#include "platform/graphics/compositing/PaintChunksToCcLayer.h"
#include "platform/graphics/paint/DrawingDisplayItem.h"
#include "platform/graphics/paint/GeometryMapper.h"
#include "platform/graphics/paint/PaintArtifact.h"
#include "platform/graphics/paint/PaintChunk.h"
#include "platform/graphics/paint/RasterInvalidationTracking.h"
#include "platform/json/JSONValues.h"
#include "platform/wtf/Optional.h"

namespace blink {

void ContentLayerClientImpl::SetTracksRasterInvalidations(bool should_track) {
  if (should_track) {
    raster_invalidation_tracking_info_ =
        WTF::MakeUnique<RasterInvalidationTrackingInfo>();

    for (const auto& info : paint_chunks_info_) {
      raster_invalidation_tracking_info_->old_client_debug_names.Set(
          &info.id.client, info.id.client.DebugName());
    }
  } else if (!RuntimeEnabledFeatures::PaintUnderInvalidationCheckingEnabled()) {
    raster_invalidation_tracking_info_ = nullptr;
  } else if (raster_invalidation_tracking_info_) {
    raster_invalidation_tracking_info_->tracking.invalidations.clear();
  }
}

const Vector<RasterInvalidationInfo>&
ContentLayerClientImpl::TrackedRasterInvalidations() const {
  return raster_invalidation_tracking_info_->tracking.invalidations;
}

static int GetTransformId(const TransformPaintPropertyNode* transform,
                          ContentLayerClientImpl::LayerAsJSONContext& context) {
  if (!transform)
    return 0;

  auto it = context.transform_id_map.find(transform);
  if (it != context.transform_id_map.end())
    return it->value;

  if ((transform->Matrix().IsIdentity() && !transform->RenderingContextId()) ||
      // Don't output scroll translations in transform tree.
      transform->ScrollNode()) {
    context.transform_id_map.Set(transform, 0);
    return 0;
  }

  int parent_id = GetTransformId(transform->Parent(), context);
  auto json = JSONObject::Create();
  int transform_id = context.next_transform_id++;
  json->SetInteger("id", transform_id);
  if (parent_id)
    json->SetInteger("parent", parent_id);

  if (!transform->Matrix().IsIdentity())
    json->SetArray("transform", TransformAsJSONArray(transform->Matrix()));

  if (!transform->Matrix().IsIdentityOrTranslation())
    json->SetArray("origin", PointAsJSONArray(transform->Origin()));

  if (!transform->FlattensInheritedTransform())
    json->SetBoolean("flattenInheritedTransform", false);

  if (auto rendering_context = transform->RenderingContextId()) {
    auto it = context.rendering_context_map.find(rendering_context);
    int rendering_id = context.rendering_context_map.size() + 1;
    if (it == context.rendering_context_map.end())
      context.rendering_context_map.Set(rendering_context, rendering_id);
    else
      rendering_id = it->value;

    json->SetInteger("renderingContext", rendering_id);
  }

  if (!context.transforms_json)
    context.transforms_json = JSONArray::Create();
  context.transforms_json->PushObject(std::move(json));

  return transform_id;
}

// This is the SPv2 version of GraphicsLayer::LayerAsJSONInternal().
std::unique_ptr<JSONObject> ContentLayerClientImpl::LayerAsJSON(
    LayerAsJSONContext& context) const {
  std::unique_ptr<JSONObject> json = JSONObject::Create();
  json->SetString("name", debug_name_);

  FloatPoint position(cc_picture_layer_->offset_to_transform_parent().x(),
                      cc_picture_layer_->offset_to_transform_parent().y());
  if (position != FloatPoint())
    json->SetArray("position", PointAsJSONArray(position));

  IntSize bounds(cc_picture_layer_->bounds().width(),
                 cc_picture_layer_->bounds().height());
  if (!bounds.IsEmpty())
    json->SetArray("bounds", SizeAsJSONArray(bounds));

  if (cc_picture_layer_->contents_opaque())
    json->SetBoolean("contentsOpaque", true);

  if (!cc_picture_layer_->DrawsContent())
    json->SetBoolean("drawsContent", false);

  if (!cc_picture_layer_->double_sided())
    json->SetString("backfaceVisibility", "hidden");

  Color background_color(cc_picture_layer_->background_color());
  if (background_color.Alpha()) {
    json->SetString("backgroundColor",
                    background_color.NameForLayoutTreeAsText());
  }

#ifndef NDEBUG
  if (context.flags & kLayerTreeIncludesDebugInfo)
    json->SetValue("paintChunkContents", paint_chunk_debug_data_->Clone());
#endif

  if ((context.flags & kLayerTreeIncludesPaintInvalidations) &&
      raster_invalidation_tracking_info_)
    raster_invalidation_tracking_info_->tracking.AsJSON(json.get());

  if (int transform_id = GetTransformId(layer_state_.Transform(), context))
    json->SetInteger("transform", transform_id);

  return json;
}

IntRect ContentLayerClientImpl::MapRasterInvalidationRectFromChunkToLayer(
    const FloatRect& r,
    const PaintChunk& chunk) const {
  FloatClipRect rect(r);
  GeometryMapper::LocalToAncestorVisualRect(
      chunk.properties.property_tree_state, layer_state_, rect);
  if (rect.Rect().IsEmpty())
    return IntRect();

  // Now rect is in the space of the containing transform node of pending_layer,
  // so need to subtract off the layer offset.
  rect.Rect().Move(-layer_bounds_.x(), -layer_bounds_.y());
  rect.Rect().Inflate(chunk.outset_for_raster_effects);
  return EnclosingIntRect(rect.Rect());
}

size_t ContentLayerClientImpl::MatchNewChunkToOldChunk(
    const PaintChunk& new_chunk,
    size_t old_index) {
  if (paint_chunks_info_[old_index].Matches(new_chunk))
    return old_index;

  size_t i = old_index;
  do {
    ++i;
    if (i == paint_chunks_info_.size())
      i = 0;
    if (paint_chunks_info_[i].Matches(new_chunk))
      return i;
  } while (i != old_index);

  return kNotFound;
}

// Generates raster invalidations by checking changes (appearing, disappearing,
// reordering, property changes) of chunks. The logic is similar to
// PaintController::GenerateRasterInvalidations(). The complexity is between
// O(n) and O(m*n) where m and n are the numbers of old and new chunks,
// respectively. Normally both m and n are small numbers. The best caseis that
// all old chunks have matching new chunks in the same order. The worst case is
// that no matching chunks except the first one (which always matches otherwise
// we won't reuse the ContentLayerClientImpl), which is rare. In common cases
// that most of the chunks can be matched in-order, the complexity is slightly
// larger than O(n).
void ContentLayerClientImpl::GenerateRasterInvalidations(
    const Vector<const PaintChunk*>& new_chunks,
    const Vector<PaintChunkInfo>& new_chunks_info) {
  Vector<bool> old_chunks_matched;
  old_chunks_matched.resize(paint_chunks_info_.size());
  size_t old_index = 0;
  for (size_t new_index = 0; new_index < new_chunks.size(); ++new_index) {
    const auto& new_chunk = *new_chunks[new_index];
    const auto& new_chunk_info = new_chunks_info[new_index];

    if (!new_chunk.is_cacheable) {
      InvalidateRasterForNewChunk(new_chunk_info,
                                  PaintInvalidationReason::kChunkUncacheable);
      continue;
    }

    size_t matched = MatchNewChunkToOldChunk(new_chunk, old_index);
    if (matched == kNotFound) {
      // The new chunk doesn't match any old chunk.
      InvalidateRasterForNewChunk(new_chunk_info,
                                  PaintInvalidationReason::kAppeared);
      continue;
    }

    old_chunks_matched[matched] = true;

    bool properties_changed =
        new_chunk.properties != paint_chunks_info_[matched].properties ||
        new_chunk.properties.property_tree_state.Changed(layer_state_);
    if (!properties_changed && matched == old_index) {
      // Add the raster invalidations found by PaintController within the chunk.
      AddDisplayItemRasterInvalidations(new_chunk);
    } else {
      // Invalidate both old and new bounds of the chunk if the chunk's paint
      // properties changed, or is moved backward and may expose area that was
      // previously covered by it.
      const auto& old_chunks_info = paint_chunks_info_[matched];
      PaintInvalidationReason reason =
          properties_changed ? PaintInvalidationReason::kPaintProperty
                             : PaintInvalidationReason::kChunkReordered;
      InvalidateRasterForOldChunk(old_chunks_info, reason);
      if (old_chunks_info.bounds_in_layer != new_chunk_info.bounds_in_layer)
        InvalidateRasterForNewChunk(new_chunk_info, reason);
      // Ignore the display item raster invalidations because we have fully
      // invalidated the chunk.
    }

    old_index = matched + 1;
    if (old_index == paint_chunks_info_.size())
      old_index = 0;
  }

  // Invalidate remaining unmatched (disappeared or uncacheable) old chunks.
  for (size_t i = 0; i < paint_chunks_info_.size(); ++i) {
    if (old_chunks_matched[i])
      continue;
    InvalidateRasterForOldChunk(
        paint_chunks_info_[i],
        paint_chunks_info_[i].is_cacheable
            ? PaintInvalidationReason::kDisappeared
            : PaintInvalidationReason::kChunkUncacheable);
  }
}

void ContentLayerClientImpl::AddDisplayItemRasterInvalidations(
    const PaintChunk& chunk) {
  DCHECK(chunk.raster_invalidation_tracking.IsEmpty() ||
         chunk.raster_invalidation_rects.size() ==
             chunk.raster_invalidation_tracking.size());

  for (size_t i = 0; i < chunk.raster_invalidation_rects.size(); ++i) {
    auto rect = MapRasterInvalidationRectFromChunkToLayer(
        chunk.raster_invalidation_rects[i], chunk);
    if (rect.IsEmpty())
      continue;
    cc_picture_layer_->SetNeedsDisplayRect(rect);

    if (!chunk.raster_invalidation_tracking.IsEmpty()) {
      const auto& info = chunk.raster_invalidation_tracking[i];
      raster_invalidation_tracking_info_->tracking.AddInvalidation(
          info.client, info.client_debug_name, rect, info.reason);
    }
  }
}

void ContentLayerClientImpl::InvalidateRasterForNewChunk(
    const PaintChunkInfo& info,
    PaintInvalidationReason reason) {
  cc_picture_layer_->SetNeedsDisplayRect(info.bounds_in_layer);

  if (raster_invalidation_tracking_info_) {
    raster_invalidation_tracking_info_->tracking.AddInvalidation(
        &info.id.client, info.id.client.DebugName(), info.bounds_in_layer,
        reason);
  }
}

void ContentLayerClientImpl::InvalidateRasterForOldChunk(
    const PaintChunkInfo& info,
    PaintInvalidationReason reason) {
  cc_picture_layer_->SetNeedsDisplayRect(info.bounds_in_layer);

  if (raster_invalidation_tracking_info_) {
    raster_invalidation_tracking_info_->tracking.AddInvalidation(
        &info.id.client,
        raster_invalidation_tracking_info_->old_client_debug_names.at(
            &info.id.client),
        info.bounds_in_layer, reason);
  }
}

void ContentLayerClientImpl::InvalidateRasterForWholeLayer() {
  IntRect rect(0, 0, layer_bounds_.width(), layer_bounds_.height());
  cc_picture_layer_->SetNeedsDisplayRect(rect);

  if (raster_invalidation_tracking_info_) {
    raster_invalidation_tracking_info_->tracking.AddInvalidation(
        &paint_chunks_info_[0].id.client, debug_name_, rect,
        PaintInvalidationReason::kFullLayer);
  }
}

static SkColor DisplayItemBackgroundColor(const DisplayItem& item) {
  if (item.GetType() != DisplayItem::kBoxDecorationBackground &&
      item.GetType() != DisplayItem::kDocumentBackground)
    return SK_ColorTRANSPARENT;

  const auto& drawing_item = static_cast<const DrawingDisplayItem&>(item);
  const auto record = drawing_item.GetPaintRecord();
  if (!record)
    return SK_ColorTRANSPARENT;

  for (cc::PaintOpBuffer::Iterator it(record.get()); it; ++it) {
    const auto* op = *it;
    if (op->GetType() == cc::PaintOpType::DrawRect ||
        op->GetType() == cc::PaintOpType::DrawRRect) {
      const auto& flags = static_cast<const cc::PaintOpWithFlags*>(op)->flags;
      // Skip op with looper which may modify the color.
      if (!flags.getLooper() && flags.getStyle() == cc::PaintFlags::kFill_Style)
        return flags.getColor();
    }
  }
  return SK_ColorTRANSPARENT;
}

scoped_refptr<cc::PictureLayer> ContentLayerClientImpl::UpdateCcPictureLayer(
    const PaintArtifact& paint_artifact,
    const gfx::Rect& layer_bounds,
    const Vector<const PaintChunk*>& paint_chunks,
    const PropertyTreeState& layer_state) {
  // TODO(wangxianzhu): Avoid calling DebugName() in official release build.
  debug_name_ = paint_chunks[0]->id.client.DebugName();

#ifndef NDEBUG
  paint_chunk_debug_data_ = JSONArray::Create();
  for (const auto* chunk : paint_chunks) {
    auto json = JSONObject::Create();
    json->SetArray("displayItems",
                   paint_artifact.GetDisplayItemList().SubsequenceAsJSON(
                       chunk->begin_index, chunk->end_index,
                       DisplayItemList::kSkipNonDrawings |
                           DisplayItemList::kShownOnlyDisplayItemTypes));
    json->SetString("propertyTreeState",
                    chunk->properties.property_tree_state.ToTreeString());
    paint_chunk_debug_data_->PushObject(std::move(json));
  }
#endif

  if (RuntimeEnabledFeatures::PaintUnderInvalidationCheckingEnabled() &&
      !raster_invalidation_tracking_info_) {
    raster_invalidation_tracking_info_ =
        WTF::MakeUnique<RasterInvalidationTrackingInfo>();
  }

  bool layer_bounds_was_empty = layer_bounds_.IsEmpty();
  bool layer_origin_changed = layer_bounds_.origin() != layer_bounds.origin();
  bool layer_state_changed = layer_state_ != layer_state;
  layer_state_ = layer_state;
  layer_bounds_ = layer_bounds;
  cc_picture_layer_->SetBounds(layer_bounds.size());
  cc_picture_layer_->SetIsDrawable(true);

  Vector<PaintChunkInfo> new_chunks_info;
  new_chunks_info.ReserveCapacity(paint_chunks.size());
  for (const auto* chunk : paint_chunks) {
    new_chunks_info.push_back(PaintChunkInfo(
        MapRasterInvalidationRectFromChunkToLayer(chunk->bounds, *chunk),
        *chunk));
    if (raster_invalidation_tracking_info_) {
      raster_invalidation_tracking_info_->new_client_debug_names.insert(
          &chunk->id.client, chunk->id.client.DebugName());
    }
  }

  if (!layer_bounds_was_empty && !layer_bounds_.IsEmpty()) {
    if (layer_origin_changed || layer_state_changed)
      InvalidateRasterForWholeLayer();
    else
      GenerateRasterInvalidations(paint_chunks, new_chunks_info);
  }

  Optional<RasterUnderInvalidationCheckingParams> params;
  if (RuntimeEnabledFeatures::PaintUnderInvalidationCheckingEnabled()) {
    params.emplace(raster_invalidation_tracking_info_->tracking,
                   IntRect(0, 0, layer_bounds_.width(), layer_bounds_.height()),
                   debug_name_);
  }
  cc_display_item_list_ = PaintChunksToCcLayer::Convert(
      paint_chunks, layer_state, layer_bounds.OffsetFromOrigin(),
      paint_artifact.GetDisplayItemList(),
      cc::DisplayItemList::kTopLevelDisplayItemList,
      params ? &*params : nullptr);

  if (paint_chunks[0]->size()) {
    cc_picture_layer_->SetBackgroundColor(DisplayItemBackgroundColor(
        paint_artifact.GetDisplayItemList()[paint_chunks[0]->begin_index]));
  }

  paint_chunks_info_.clear();
  std::swap(paint_chunks_info_, new_chunks_info);
  if (raster_invalidation_tracking_info_) {
    raster_invalidation_tracking_info_->old_client_debug_names.clear();
    std::swap(raster_invalidation_tracking_info_->old_client_debug_names,
              raster_invalidation_tracking_info_->new_client_debug_names);
  }

  return cc_picture_layer_;
}

}  // namespace blink