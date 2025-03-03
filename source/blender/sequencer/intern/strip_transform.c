/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved.
 *           2003-2009 Blender Foundation.
 *           2005-2006 Peter Schlaile <peter [at] schlaile [dot] de> */

/** \file
 * \ingroup bke
 */

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BKE_scene.h"
#include "BKE_sound.h"

#include "SEQ_animation.h"
#include "SEQ_channels.h"
#include "SEQ_effects.h"
#include "SEQ_iterator.h"
#include "SEQ_relations.h"
#include "SEQ_sequencer.h"
#include "SEQ_time.h"
#include "SEQ_transform.h"

#include "strip_time.h"
#include "sequencer.h"

#include "CLG_log.h"

static CLG_LogRef LOG = {"seq.strip_transform"};

static int seq_tx_get_start(Sequence *seq)
{
  return seq->start;
}
static int seq_tx_get_end(Sequence *seq)
{
  return seq->start + seq->len;
}

bool SEQ_transform_single_image_check(Sequence *seq)
{
  return ((seq->len == 1) &&
          (seq->type == SEQ_TYPE_IMAGE ||
           ((seq->type & SEQ_TYPE_EFFECT) && SEQ_effect_get_num_inputs(seq->type) == 0)));
}

bool SEQ_transform_seqbase_isolated_sel_check(ListBase *seqbase)
{
  Sequence *seq;
  /* is there more than 1 select */
  bool ok = false;

  for (seq = seqbase->first; seq; seq = seq->next) {
    if (seq->flag & SELECT) {
      ok = true;
      break;
    }
  }

  if (ok == false) {
    return false;
  }

  /* test relationships */
  for (seq = seqbase->first; seq; seq = seq->next) {
    if ((seq->type & SEQ_TYPE_EFFECT) == 0) {
      continue;
    }

    if (seq->flag & SELECT) {
      if ((seq->seq1 && (seq->seq1->flag & SELECT) == 0) ||
          (seq->seq2 && (seq->seq2->flag & SELECT) == 0) ||
          (seq->seq3 && (seq->seq3->flag & SELECT) == 0)) {
        return false;
      }
    }
    else {
      if ((seq->seq1 && (seq->seq1->flag & SELECT)) || (seq->seq2 && (seq->seq2->flag & SELECT)) ||
          (seq->seq3 && (seq->seq3->flag & SELECT))) {
        return false;
      }
    }
  }

  return true;
}

void SEQ_transform_handle_xlimits(const Scene *scene, Sequence *seq, int leftflag, int rightflag)
{
  if (leftflag) {
    if (SEQ_time_left_handle_frame_get(seq) >= SEQ_time_right_handle_frame_get(seq)) {
      SEQ_time_left_handle_frame_set(scene, seq, SEQ_time_right_handle_frame_get(seq) - 1);
    }

    if (SEQ_transform_single_image_check(seq) == 0) {
      if (SEQ_time_left_handle_frame_get(seq) >= seq_tx_get_end(seq)) {
        SEQ_time_left_handle_frame_set(scene, seq, seq_tx_get_end(seq) - 1);
      }

      /* TODO: This doesn't work at the moment. */
#if 0
      if (seq_tx_get_start(seq) >= seq_tx_get_final_right(seq, 0)) {
        int ofs;
        ofs = seq_tx_get_start(seq) - seq_tx_get_final_right(seq, 0);
        seq->start -= ofs;
        seq_tx_set_final_left(seq, seq_tx_get_final_left(seq, 0) + ofs);
      }
#endif
    }
  }

  if (rightflag) {
    if (SEQ_time_right_handle_frame_get(seq) <= SEQ_time_left_handle_frame_get(seq)) {
      SEQ_time_right_handle_frame_set(scene, seq, SEQ_time_left_handle_frame_get(seq) + 1);
    }

    if (SEQ_transform_single_image_check(seq) == 0) {
      if (SEQ_time_right_handle_frame_get(seq) <= seq_tx_get_start(seq)) {
        SEQ_time_right_handle_frame_set(scene, seq, seq_tx_get_start(seq) + 1);
      }
    }
  }

  /* sounds cannot be extended past their endpoints */
  if (seq->type == SEQ_TYPE_SOUND_RAM) {
    CLAMP(seq->startofs, 0, MAXFRAME);
    CLAMP(seq->endofs, 0, MAXFRAME);
  }
}

void SEQ_transform_fix_single_image_seq_offsets(const Scene *scene, Sequence *seq)
{
  int left, start, offset;
  if (!SEQ_transform_single_image_check(seq)) {
    return;
  }

  /* make sure the image is always at the start since there is only one,
   * adjusting its start should be ok */
  left = SEQ_time_left_handle_frame_get(seq);
  start = seq->start;
  if (start != left) {
    offset = left - start;
    SEQ_time_left_handle_frame_set(scene, seq, SEQ_time_left_handle_frame_get(seq) - offset);
    SEQ_time_right_handle_frame_set(scene, seq, SEQ_time_right_handle_frame_get(seq) - offset);
    seq->start += offset;
  }
}

bool SEQ_transform_sequence_can_be_translated(Sequence *seq)
{
  return !(seq->type & SEQ_TYPE_EFFECT) || (SEQ_effect_get_num_inputs(seq->type) == 0);
}

bool SEQ_transform_test_overlap_seq_seq(Sequence *seq1, Sequence *seq2)
{
  return (seq1 != seq2 && seq1->machine == seq2->machine &&
          ((SEQ_time_right_handle_frame_get(seq1) <= SEQ_time_left_handle_frame_get(seq2)) ||
           (SEQ_time_left_handle_frame_get(seq1) >= SEQ_time_right_handle_frame_get(seq2))) == 0);
}

bool SEQ_transform_test_overlap(ListBase *seqbasep, Sequence *test)
{
  Sequence *seq;

  seq = seqbasep->first;
  while (seq) {
    if (SEQ_transform_test_overlap_seq_seq(test, seq)) {
      return true;
    }

    seq = seq->next;
  }
  return false;
}

void SEQ_transform_translate_sequence(Scene *evil_scene, Sequence *seq, int delta)
{
  if (delta == 0) {
    return;
  }

  /* Meta strips requires special handling: their content is to be translated, and then frame range
   * of the meta is to be updated for the updated content. */
  if (seq->type == SEQ_TYPE_META) {
    Sequence *seq_child;
    for (seq_child = seq->seqbase.first; seq_child; seq_child = seq_child->next) {
      SEQ_transform_translate_sequence(evil_scene, seq_child, delta);
    }
    /* Move meta start/end points. */
    SEQ_time_left_handle_frame_set(evil_scene, seq, SEQ_time_left_handle_frame_get(seq) + delta);
    SEQ_time_right_handle_frame_set(evil_scene, seq, SEQ_time_right_handle_frame_get(seq) + delta);
  }
  else { /* All other strip types. */
    seq->start += delta;
    /* Only to make files usable in older versions. */
    seq->startdisp = SEQ_time_left_handle_frame_get(seq); 
    seq->enddisp = SEQ_time_right_handle_frame_get(seq);
  }

  SEQ_offset_animdata(evil_scene, seq, delta);
  SEQ_time_update_meta_strip_range(evil_scene, seq_sequence_lookup_meta_by_seq(evil_scene, seq));
  seq_time_update_effects_strip_range(evil_scene,
                                      seq_sequence_lookup_effects_by_seq(evil_scene, seq));
}

bool SEQ_transform_seqbase_shuffle_ex(ListBase *seqbasep,
                                      Sequence *test,
                                      Scene *evil_scene,
                                      int channel_delta)
{
  const int orig_machine = test->machine;
  BLI_assert(ELEM(channel_delta, -1, 1));

  test->machine += channel_delta;
  while (SEQ_transform_test_overlap(seqbasep, test)) {
    if ((channel_delta > 0) ? (test->machine >= MAXSEQ) : (test->machine < 1)) {
      break;
    }

    test->machine += channel_delta;
  }

  if (!SEQ_valid_strip_channel(test)) {
    /* Blender 2.4x would remove the strip.
     * nicer to move it to the end */

    Sequence *seq;
    int new_frame = SEQ_time_right_handle_frame_get(test);

    for (seq = seqbasep->first; seq; seq = seq->next) {
      if (seq->machine == orig_machine) {
        new_frame = max_ii(new_frame, SEQ_time_right_handle_frame_get(seq));
      }
    }

    test->machine = orig_machine;
    new_frame = new_frame +
                (test->start - SEQ_time_left_handle_frame_get(test)); /* adjust by the startdisp */
    SEQ_transform_translate_sequence(evil_scene, test, new_frame - test->start);
    return false;
  }

  return true;
}

bool SEQ_transform_seqbase_shuffle(ListBase *seqbasep, Sequence *test, Scene *evil_scene)
{
  return SEQ_transform_seqbase_shuffle_ex(seqbasep, test, evil_scene, 1);
}

static bool shuffle_seq_test_overlap(const Sequence *seq1, const Sequence *seq2, const int offset)
{
  return (
      seq1 != seq2 && seq1->machine == seq2->machine &&
      ((SEQ_time_right_handle_frame_get(seq1) + offset <= SEQ_time_left_handle_frame_get(seq2)) ||
       (SEQ_time_left_handle_frame_get(seq1) + offset >= SEQ_time_right_handle_frame_get(seq2))) ==
          0);
}

static int shuffle_seq_time_offset_get(SeqCollection *strips_to_shuffle,
                                       ListBase *seqbasep,
                                       char dir)
{
  int offset = 0;
  Sequence *seq;
  bool all_conflicts_resolved = false;

  while (!all_conflicts_resolved) {
    all_conflicts_resolved = true;
    SEQ_ITERATOR_FOREACH (seq, strips_to_shuffle) {
      LISTBASE_FOREACH (Sequence *, seq_other, seqbasep) {
        if (!shuffle_seq_test_overlap(seq, seq_other, offset)) {
          continue;
        }
        if (SEQ_relation_is_effect_of_strip(seq_other, seq)) {
          continue;
        }
        if (UNLIKELY(SEQ_collection_has_strip(seq_other, strips_to_shuffle))) {
          CLOG_WARN(&LOG,
                    "Strip overlaps with itself or another strip, that is to be shuffled. "
                    "This should never happen.");
          continue;
        }

        all_conflicts_resolved = false;

        if (dir == 'L') {
          offset = min_ii(offset,
                          SEQ_time_left_handle_frame_get(seq_other) -
                              SEQ_time_right_handle_frame_get(seq));
        }
        else {
          offset = max_ii(offset,
                          SEQ_time_right_handle_frame_get(seq_other) -
                              SEQ_time_left_handle_frame_get(seq));
        }
      }
    }
  }

  return offset;
}

bool SEQ_transform_seqbase_shuffle_time(SeqCollection *strips_to_shuffle,
                                        ListBase *seqbasep,
                                        Scene *evil_scene,
                                        ListBase *markers,
                                        const bool use_sync_markers)
{
  int offset_l = shuffle_seq_time_offset_get(strips_to_shuffle, seqbasep, 'L');
  int offset_r = shuffle_seq_time_offset_get(strips_to_shuffle, seqbasep, 'R');
  int offset = (-offset_l < offset_r) ? offset_l : offset_r;

  if (offset) {
    Sequence *seq;
    SEQ_ITERATOR_FOREACH (seq, strips_to_shuffle) {
      SEQ_transform_translate_sequence(evil_scene, seq, offset);
      seq->flag &= ~SEQ_OVERLAP;
    }

    if (use_sync_markers && !(evil_scene->toolsettings->lock_markers) && (markers != NULL)) {
      TimeMarker *marker;
      /* affect selected markers - it's unlikely that we will want to affect all in this way? */
      for (marker = markers->first; marker; marker = marker->next) {
        if (marker->flag & SELECT) {
          marker->frame += offset;
        }
      }
    }
  }

  return offset ? false : true;
}

void SEQ_transform_offset_after_frame(Scene *scene,
                                      ListBase *seqbase,
                                      const int delta,
                                      const int timeline_frame)
{
  LISTBASE_FOREACH (Sequence *, seq, seqbase) {
    if (SEQ_time_left_handle_frame_get(seq) >= timeline_frame) {
      SEQ_transform_translate_sequence(scene, seq, delta);
      SEQ_relations_invalidate_cache_preprocessed(scene, seq);
    }
  }

  if (!scene->toolsettings->lock_markers) {
    LISTBASE_FOREACH (TimeMarker *, marker, &scene->markers) {
      if (marker->frame >= timeline_frame) {
        marker->frame += delta;
      }
    }
  }
}

bool SEQ_transform_is_locked(ListBase *channels, Sequence *seq)
{
  SeqTimelineChannel *channel = SEQ_channel_get_by_index(channels, seq->machine);
  return seq->flag & SEQ_LOCK ||
         (SEQ_channel_is_locked(channel) && ((seq->flag & SEQ_IGNORE_CHANNEL_LOCK) == 0));
}

void SEQ_image_transform_mirror_factor_get(const Sequence *seq, float r_mirror[2])
{
  r_mirror[0] = 1.0f;
  r_mirror[1] = 1.0f;

  if ((seq->flag & SEQ_FLIPX) != 0) {
    r_mirror[0] = -1.0f;
  }
  if ((seq->flag & SEQ_FLIPY) != 0) {
    r_mirror[1] = -1.0f;
  }
}

void SEQ_image_transform_origin_offset_pixelspace_get(const Scene *scene,
                                                      const Sequence *seq,
                                                      float r_origin[2])
{
  float image_size[2];
  StripElem *strip_elem = seq->strip->stripdata;
  if (strip_elem == NULL) {
    image_size[0] = scene->r.xsch;
    image_size[1] = scene->r.ysch;
  }
  else {
    image_size[0] = strip_elem->orig_width;
    image_size[1] = strip_elem->orig_height;
  }

  const StripTransform *transform = seq->strip->transform;
  r_origin[0] = (image_size[0] * transform->origin[0]) - (image_size[0] * 0.5f) + transform->xofs;
  r_origin[1] = (image_size[1] * transform->origin[1]) - (image_size[1] * 0.5f) + transform->yofs;

  const float viewport_pixel_aspect[2] = {scene->r.xasp / scene->r.yasp, 1.0f};
  float mirror[2];
  SEQ_image_transform_mirror_factor_get(seq, mirror);
  mul_v2_v2(r_origin, mirror);
  mul_v2_v2(r_origin, viewport_pixel_aspect);
}

static void seq_image_transform_quad_get_ex(const Scene *scene,
                                            const Sequence *seq,
                                            bool apply_rotation,
                                            float r_quad[4][2])
{
  StripTransform *transform = seq->strip->transform;
  StripCrop *crop = seq->strip->crop;

  int image_size[2] = {scene->r.xsch, scene->r.ysch};
  if (ELEM(seq->type, SEQ_TYPE_MOVIE, SEQ_TYPE_IMAGE)) {
    image_size[0] = seq->strip->stripdata->orig_width;
    image_size[1] = seq->strip->stripdata->orig_height;
  }

  float transform_matrix[4][4];
  float rotation_matrix[3][3];
  axis_angle_to_mat3_single(rotation_matrix, 'Z', apply_rotation ? transform->rotation : 0.0f);
  loc_rot_size_to_mat4(transform_matrix,
                       (const float[]){transform->xofs, transform->yofs, 0.0f},
                       rotation_matrix,
                       (const float[]){transform->scale_x, transform->scale_y, 1.0f});
  const float origin[2] = {image_size[0] * transform->origin[0],
                           image_size[1] * transform->origin[1]};
  const float pivot[3] = {origin[0] - (image_size[0] / 2), origin[1] - (image_size[1] / 2), 0.0f};
  transform_pivot_set_m4(transform_matrix, pivot);

  float quad_temp[4][3];
  for (int i = 0; i < 4; i++) {
    zero_v2(quad_temp[i]);
  }

  quad_temp[0][0] = (image_size[0] / 2) - crop->right;
  quad_temp[0][1] = (image_size[1] / 2) - crop->top;
  quad_temp[1][0] = (image_size[0] / 2) - crop->right;
  quad_temp[1][1] = (-image_size[1] / 2) + crop->bottom;
  quad_temp[2][0] = (-image_size[0] / 2) + crop->left;
  quad_temp[2][1] = (-image_size[1] / 2) + crop->bottom;
  quad_temp[3][0] = (-image_size[0] / 2) + crop->left;
  quad_temp[3][1] = (image_size[1] / 2) - crop->top;

  float mirror[2];
  SEQ_image_transform_mirror_factor_get(seq, mirror);

  const float viewport_pixel_aspect[2] = {scene->r.xasp / scene->r.yasp, 1.0f};

  for (int i = 0; i < 4; i++) {
    mul_m4_v3(transform_matrix, quad_temp[i]);
    mul_v2_v2(quad_temp[i], mirror);
    mul_v2_v2(quad_temp[i], viewport_pixel_aspect);
    copy_v2_v2(r_quad[i], quad_temp[i]);
  }
}

void SEQ_image_transform_quad_get(const Scene *scene,
                                  const Sequence *seq,
                                  bool apply_rotation,
                                  float r_quad[4][2])
{
  seq_image_transform_quad_get_ex(scene, seq, apply_rotation, r_quad);
}

void SEQ_image_transform_final_quad_get(const Scene *scene,
                                        const Sequence *seq,
                                        float r_quad[4][2])
{
  seq_image_transform_quad_get_ex(scene, seq, true, r_quad);
}

void SEQ_image_preview_unit_to_px(const Scene *scene, const float co_src[2], float co_dst[2])
{
  co_dst[0] = co_src[0] * scene->r.xsch;
  co_dst[1] = co_src[1] * scene->r.ysch;
}

void SEQ_image_preview_unit_from_px(const Scene *scene, const float co_src[2], float co_dst[2])
{
  co_dst[0] = co_src[0] / scene->r.xsch;
  co_dst[1] = co_src[1] / scene->r.ysch;
}

void SEQ_image_transform_bounding_box_from_collection(
    Scene *scene, SeqCollection *strips, bool apply_rotation, float r_min[2], float r_max[2])
{
  Sequence *seq;

  INIT_MINMAX2(r_min, r_max);
  SEQ_ITERATOR_FOREACH (seq, strips) {
    float quad[4][2];
    SEQ_image_transform_quad_get(scene, seq, apply_rotation, quad);
    for (int i = 0; i < 4; i++) {
      minmax_v2v2_v2(r_min, r_max, quad[i]);
    }
  }
}
