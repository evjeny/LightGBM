/*!
 * Copyright (c) 2016 Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See LICENSE file in the project root for license information.
 */
#include <LightGBM/dataset.h>

#include <LightGBM/feature_group.h>
#include <LightGBM/utils/array_args.h>
#include <LightGBM/utils/openmp_wrapper.h>
#include <LightGBM/utils/threading.h>

#include <limits>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <unordered_map>


namespace LightGBM {


#ifdef TIMETAG
std::chrono::duration<double, std::milli> dense_bin_time;
std::chrono::duration<double, std::milli> sparse_bin_time;
std::chrono::duration<double, std::milli> sparse_hist_prep_time;
std::chrono::duration<double, std::milli> sparse_hist_merge_time;
#endif  // TIMETAG

const char* Dataset::binary_file_token = "______LightGBM_Binary_File_Token______\n";

Dataset::Dataset() {
  data_filename_ = "noname";
  num_data_ = 0;
  is_finish_load_ = false;
}

Dataset::Dataset(data_size_t num_data) {
  CHECK(num_data > 0);
  data_filename_ = "noname";
  num_data_ = num_data;
  metadata_.Init(num_data_, NO_SPECIFIC, NO_SPECIFIC);
  is_finish_load_ = false;
  group_bin_boundaries_.push_back(0);
}

Dataset::~Dataset() {
  #ifdef TIMETAG
  Log::Info("Dataset::dense_bin_time costs %f", dense_bin_time * 1e-3);
  Log::Info("Dataset::sparse_bin_time costs %f", sparse_bin_time * 1e-3);
  Log::Info("Dataset::sparse_hist_prep_time costs %f", sparse_hist_prep_time * 1e-3);
  Log::Info("Dataset::sparse_hist_merge_time costs %f", sparse_hist_merge_time * 1e-3);
  #endif
}

std::vector<std::vector<int>> NoGroup(
  const std::vector<int>& used_features) {
  std::vector<std::vector<int>> features_in_group;
  features_in_group.resize(used_features.size());
  for (size_t i = 0; i < used_features.size(); ++i) {
    features_in_group[i].emplace_back(used_features[i]);
  }
  return features_in_group;
}

int GetConfilctCount(const std::vector<uint8_t>& mark, const int* indices, int num_indices, data_size_t max_cnt, int max_feature_cnt) {
  int ret = 0;
  for (int i = 0; i < num_indices; ++i) {
    if (mark[indices[i]]) {
      ++ret;
      if (mark[indices[i]] + 1 > max_feature_cnt) {
        return -1;
      }
    }
    if (ret >= max_cnt) {
      return -1;
    }
  }
  return ret;
}

void MarkUsed(std::vector<uint8_t>* mark, const int* indices, data_size_t num_indices) {
  auto& ref_mark = *mark;
  for (int i = 0; i < num_indices; ++i) {
    ref_mark[indices[i]] += 1;
  }
}

std::vector<int> FixSampleIndices(const BinMapper* bin_mapper, int num_total_samples, int num_indices, const int* sample_indices, const double* sample_values) {
  std::vector<int> ret;
  if (bin_mapper->GetDefaultBin() == bin_mapper->GetMostFreqBin()) {
    return ret;
  }
  int i = 0, j = 0;
  while (i < num_total_samples) {
    if (j < num_indices && sample_indices[j] < i) {
      ++j;
    } else if (j < num_indices && sample_indices[j] == i) {
      if (bin_mapper->ValueToBin(sample_values[j]) != bin_mapper->GetMostFreqBin()) {
        ret.push_back(i);
      }
      ++i;
    } else {
      ret.push_back(i++);
    }
  }
  return ret;
}

std::vector<std::vector<int>> FindGroups(const std::vector<std::unique_ptr<BinMapper>>& bin_mappers,
                                         const std::vector<int>& find_order,
                                         int** sample_indices,
                                         const int* num_per_col,
                                         int num_sample_col,
                                         data_size_t total_sample_cnt,
                                         data_size_t num_data,
                                         bool is_use_gpu,
                                         std::vector<bool>* multi_val_group) {
  const int max_search_group = 100;
  const int max_bin_per_group = 256;
  const data_size_t single_val_max_conflict_cnt = static_cast<data_size_t>(total_sample_cnt / 10000);
  const data_size_t max_samples_per_multi_val_group = static_cast<data_size_t>(total_sample_cnt * 10);
  multi_val_group->clear();

  Random rand(num_data);
  std::vector<std::vector<int>> features_in_group;
  std::vector<std::vector<uint8_t>> conflict_marks;
  std::vector<data_size_t> group_used_row_cnt;
  std::vector<data_size_t> group_total_data_cnt;
  std::vector<int> group_num_bin;

  // first round: fill the single val group
  for (auto fidx : find_order) {
    bool is_filtered_feature = fidx >= num_sample_col;
    const data_size_t cur_non_zero_cnt = is_filtered_feature ? 0 : num_per_col[fidx];
    std::vector<int> available_groups;
    for (int gid = 0; gid < static_cast<int>(features_in_group.size()); ++gid) {
      auto cur_num_bin = group_num_bin[gid] + bin_mappers[fidx]->num_bin() + (bin_mappers[fidx]->GetDefaultBin() == 0 ? -1 : 0);
      if (group_total_data_cnt[gid] + cur_non_zero_cnt <= total_sample_cnt + single_val_max_conflict_cnt) {
        if (!is_use_gpu || cur_num_bin <= max_bin_per_group) {
          available_groups.push_back(gid);
        }
      }
    }
    std::vector<int> search_groups;
    if (!available_groups.empty()) {
      int last = static_cast<int>(available_groups.size()) - 1;
      auto indices = rand.Sample(last, std::min(last, max_search_group - 1));
      // always push the last group
      search_groups.push_back(available_groups.back());
      for (auto idx : indices) {
        search_groups.push_back(available_groups[idx]);
      }
    }
    int best_gid = -1;
    int best_conflict_cnt = -1;
    for (auto gid : search_groups) {
      const data_size_t rest_max_cnt = single_val_max_conflict_cnt - group_total_data_cnt[gid] + group_used_row_cnt[gid];
      const data_size_t cnt = is_filtered_feature ? 0 : GetConfilctCount(conflict_marks[gid], sample_indices[fidx], num_per_col[fidx], rest_max_cnt, 1);
      if (cnt >= 0 && cnt <= rest_max_cnt && cnt <= cur_non_zero_cnt / 2) {
        best_gid = gid;
        best_conflict_cnt = cnt;
        break;
      }
    }
    if (best_gid >= 0) {
      features_in_group[best_gid].push_back(fidx);
      group_total_data_cnt[best_gid] += cur_non_zero_cnt;
      group_used_row_cnt[best_gid] += cur_non_zero_cnt - best_conflict_cnt;
      if (!is_filtered_feature) {
        MarkUsed(&conflict_marks[best_gid], sample_indices[fidx], num_per_col[fidx]);
      }
      group_num_bin[best_gid] += bin_mappers[fidx]->num_bin() + (bin_mappers[fidx]->GetDefaultBin() == 0 ? -1 : 0);
    } else {
      features_in_group.emplace_back();
      features_in_group.back().push_back(fidx);
      conflict_marks.emplace_back(total_sample_cnt, 0);
      if (!is_filtered_feature) {
        MarkUsed(&(conflict_marks.back()), sample_indices[fidx], num_per_col[fidx]);
      }
      group_total_data_cnt.emplace_back(cur_non_zero_cnt);
      group_used_row_cnt.emplace_back(cur_non_zero_cnt);
      group_num_bin.push_back(1 + bin_mappers[fidx]->num_bin() + (bin_mappers[fidx]->GetDefaultBin() == 0 ? -1 : 0));
    }
  }

  std::vector<int> second_round_features;
  std::vector<std::vector<int>> features_in_group2;
  std::vector<std::vector<uint8_t>> conflict_marks2;
  std::vector<data_size_t> group_used_row_cnt2;
  std::vector<data_size_t> group_total_data_cnt2;
  std::vector<int> group_num_bin2;
  std::vector<bool> forced_single_val_group;

  const double dense_threshold = 0.6;
  for (int gid = 0; gid < static_cast<int>(features_in_group.size()); ++gid) {
    const double dense_rate = static_cast<double>(group_used_row_cnt[gid]) / total_sample_cnt;
    if (dense_rate >= dense_threshold) {
      features_in_group2.push_back(std::move(features_in_group[gid]));
      conflict_marks2.push_back(std::move(conflict_marks[gid]));
      group_used_row_cnt2.push_back(group_used_row_cnt[gid]);
      group_total_data_cnt2.push_back(group_total_data_cnt[gid]);
      group_num_bin2.push_back(group_num_bin[gid]);
      forced_single_val_group.push_back(true);
    } else {
      for (auto fidx : features_in_group[gid]) {
        second_round_features.push_back(fidx);
      }
    }
  }

  features_in_group = features_in_group2;
  conflict_marks = conflict_marks2;
  group_total_data_cnt = group_total_data_cnt2;
  group_used_row_cnt = group_used_row_cnt2;
  group_num_bin = group_num_bin2;
  multi_val_group->resize(features_in_group.size(), false);
  const int max_concurrent_feature_per_group = 64;
  const int max_bin_per_multi_val_group = 1 << 14;

  // second round: fill the multi-val group
  for (auto fidx : second_round_features) {
    bool is_filtered_feature = fidx >= num_sample_col;
    const int cur_non_zero_cnt = is_filtered_feature ? 0 : num_per_col[fidx];
    std::vector<int> available_groups;
    for (int gid = 0; gid < static_cast<int>(features_in_group.size()); ++gid) {
      auto cur_num_bin = group_num_bin[gid] + bin_mappers[fidx]->num_bin() + (bin_mappers[fidx]->GetDefaultBin() == 0 ? -1 : 0);
      if (multi_val_group->at(gid) && group_num_bin[gid] + cur_num_bin > max_bin_per_multi_val_group) {
        continue;
      }
      const int max_sample_cnt = forced_single_val_group[gid] ? total_sample_cnt + single_val_max_conflict_cnt : max_samples_per_multi_val_group;
      if (group_total_data_cnt[gid] + cur_non_zero_cnt <= max_sample_cnt) {
        if (!is_use_gpu || cur_num_bin <= max_bin_per_group) {
          available_groups.push_back(gid);
        }
      }
    }
    
    std::vector<int> search_groups;
    if (!available_groups.empty()) {
      int last = static_cast<int>(available_groups.size()) - 1;
      auto indices = rand.Sample(last, std::min(last, max_search_group - 1));
      // always push the last group
      search_groups.push_back(available_groups.back());
      for (auto idx : indices) {
        search_groups.push_back(available_groups[idx]);
      }
    }
    int best_gid = -1;
    int best_conflict_cnt = total_sample_cnt + 1;
    for (auto gid : search_groups) {
      int rest_max_cnt = total_sample_cnt;
      if (forced_single_val_group[gid]) {
        rest_max_cnt = std::min(rest_max_cnt, single_val_max_conflict_cnt - group_total_data_cnt[gid] + group_used_row_cnt[gid]);
      } 
      const int cnt = is_filtered_feature ? 0 : GetConfilctCount(conflict_marks[gid], sample_indices[fidx], num_per_col[fidx], rest_max_cnt, max_concurrent_feature_per_group);
      if (cnt < 0) {
        continue;
      }
      if (cnt < best_conflict_cnt || (cnt == best_conflict_cnt && (forced_single_val_group[gid] || group_total_data_cnt[best_gid] > group_total_data_cnt[gid]))) {
        best_conflict_cnt = cnt;
        best_gid = gid;
      }
      if (cnt == 0 && forced_single_val_group[gid]) { break; }
    }
    if (best_gid >= 0) {
      features_in_group[best_gid].push_back(fidx);
      group_total_data_cnt[best_gid] += cur_non_zero_cnt;
      group_used_row_cnt[best_gid] += cur_non_zero_cnt - best_conflict_cnt;
      if (!is_filtered_feature) {
        MarkUsed(&conflict_marks[best_gid], sample_indices[fidx], num_per_col[fidx]);
      }
      group_num_bin[best_gid] += bin_mappers[fidx]->num_bin() + (bin_mappers[fidx]->GetDefaultBin() == 0 ? -1 : 0);
      if (!multi_val_group->at(best_gid) && group_total_data_cnt[best_gid] - group_used_row_cnt[best_gid] > single_val_max_conflict_cnt) {
        multi_val_group->at(best_gid) = true;
      }
    } else {
      forced_single_val_group.push_back(false);
      features_in_group.emplace_back();
      features_in_group.back().push_back(fidx);
      conflict_marks.emplace_back(total_sample_cnt, 0);
      if (!is_filtered_feature) {
        MarkUsed(&(conflict_marks.back()), sample_indices[fidx], num_per_col[fidx]);
      }
      group_total_data_cnt.emplace_back(cur_non_zero_cnt);
      group_used_row_cnt.emplace_back(cur_non_zero_cnt);
      group_num_bin.push_back(1 + bin_mappers[fidx]->num_bin() + (bin_mappers[fidx]->GetDefaultBin() == 0 ? -1 : 0));
      multi_val_group->push_back(false);
    }
  }
  return features_in_group;
}

std::vector<std::vector<int>> FastFeatureBundling(const std::vector<std::unique_ptr<BinMapper>>& bin_mappers,
                                                  int** sample_indices,
                                                  double** sample_values,
                                                  const int* num_per_col,
                                                  int num_sample_col,
                                                  data_size_t total_sample_cnt,
                                                  const std::vector<int>& used_features,
                                                  data_size_t num_data,
                                                  bool is_use_gpu,
                                                  std::vector<bool>* multi_val_group) {
  std::vector<size_t> feature_non_zero_cnt;
  feature_non_zero_cnt.reserve(used_features.size());
  // put dense feature first
  for (auto fidx : used_features) {
    if (fidx < num_sample_col) {
      feature_non_zero_cnt.emplace_back(num_per_col[fidx]);
    } else {
      feature_non_zero_cnt.emplace_back(0);
    }
  }
  // sort by non zero cnt
  std::vector<int> sorted_idx;
  sorted_idx.reserve(used_features.size());
  for (int i = 0; i < static_cast<int>(used_features.size()); ++i) {
    sorted_idx.emplace_back(i);
  }
  // sort by non zero cnt, bigger first
  std::stable_sort(sorted_idx.begin(), sorted_idx.end(),
                   [&feature_non_zero_cnt](int a, int b) {
    return feature_non_zero_cnt[a] > feature_non_zero_cnt[b];
  });

  std::vector<int> feature_order_by_cnt;
  feature_order_by_cnt.reserve(sorted_idx.size());
  for (auto sidx : sorted_idx) {
    feature_order_by_cnt.push_back(used_features[sidx]);
  }

  std::vector<std::vector<int>> tmp_indices;
  std::vector<int> tmp_num_per_col(num_sample_col, 0);
  for (auto fidx : used_features) {
    if (fidx >= num_sample_col) {
      continue;
    }
    auto ret = FixSampleIndices(bin_mappers[fidx].get(), static_cast<int>(total_sample_cnt), num_per_col[fidx], sample_indices[fidx], sample_values[fidx]);
    if (!ret.empty()) {
      tmp_indices.push_back(ret);
      tmp_num_per_col[fidx] = static_cast<int>(ret.size());
      sample_indices[fidx] = tmp_indices.back().data();
    } else {
      tmp_num_per_col[fidx] = num_per_col[fidx];
    }
  }
  std::vector<bool> group_is_multi_val, group_is_multi_val2;
  auto features_in_group = FindGroups(bin_mappers, used_features, sample_indices, tmp_num_per_col.data(), num_sample_col, total_sample_cnt, num_data, is_use_gpu, &group_is_multi_val);
  auto group2 = FindGroups(bin_mappers, feature_order_by_cnt, sample_indices, tmp_num_per_col.data(), num_sample_col, total_sample_cnt, num_data, is_use_gpu, &group_is_multi_val2);

  if (features_in_group.size() > group2.size()) {
    features_in_group = group2;
    group_is_multi_val = group_is_multi_val2;
  }
  // shuffle groups
  int num_group = static_cast<int>(features_in_group.size());
  Random tmp_rand(num_data);
  for (int i = 0; i < num_group - 1; ++i) {
    int j = tmp_rand.NextShort(i + 1, num_group);
    std::swap(features_in_group[i], features_in_group[j]);
    // Use std::swap for vector<bool> will cause the wrong result..
    std::vector<bool>::swap(group_is_multi_val[i], group_is_multi_val[j]);
  }
  *multi_val_group = group_is_multi_val;
  return features_in_group;
}

void Dataset::Construct(
  std::vector<std::unique_ptr<BinMapper>>* bin_mappers,
  int num_total_features,
  const std::vector<std::vector<double>>& forced_bins,
  int** sample_non_zero_indices,
  double** sample_values,
  const int* num_per_col,
  int num_sample_col,
  size_t total_sample_cnt,
  const Config& io_config) {
  num_total_features_ = num_total_features;
  CHECK(num_total_features_ == static_cast<int>(bin_mappers->size()));
  // get num_features
  std::vector<int> used_features;
  auto& ref_bin_mappers = *bin_mappers;
  for (int i = 0; i < static_cast<int>(bin_mappers->size()); ++i) {
    if (ref_bin_mappers[i] != nullptr && !ref_bin_mappers[i]->is_trivial()) {
      used_features.emplace_back(i);
    }
  }
  if (used_features.empty()) {
    Log::Warning("There are no meaningful features, as all feature values are constant.");
  }
  auto features_in_group = NoGroup(used_features);
  std::vector<bool> group_is_multi_val(used_features.size(), false);
  if (io_config.enable_bundle && !used_features.empty()) {
    features_in_group = FastFeatureBundling(*bin_mappers,
                                            sample_non_zero_indices, sample_values, num_per_col, num_sample_col, static_cast<data_size_t>(total_sample_cnt),
                                            used_features, num_data_, io_config.device_type == std::string("gpu"), &group_is_multi_val);
  }

  num_features_ = 0;
  for (const auto& fs : features_in_group) {
    num_features_ += static_cast<int>(fs.size());
  }
  int cur_fidx = 0;
  used_feature_map_ = std::vector<int>(num_total_features_, -1);
  num_groups_ = static_cast<int>(features_in_group.size());
  real_feature_idx_.resize(num_features_);
  feature2group_.resize(num_features_);
  feature2subfeature_.resize(num_features_);
  int num_multi_val_group = 0;
  feature_need_push_zeros_.clear();
  for (int i = 0; i < num_groups_; ++i) {
    auto cur_features = features_in_group[i];
    int cur_cnt_features = static_cast<int>(cur_features.size());
    if (group_is_multi_val[i]) {
      ++num_multi_val_group;
    }
    // get bin_mappers
    std::vector<std::unique_ptr<BinMapper>> cur_bin_mappers;
    for (int j = 0; j < cur_cnt_features; ++j) {
      int real_fidx = cur_features[j];
      used_feature_map_[real_fidx] = cur_fidx;
      real_feature_idx_[cur_fidx] = real_fidx;
      feature2group_[cur_fidx] = i;
      feature2subfeature_[cur_fidx] = j;
      cur_bin_mappers.emplace_back(ref_bin_mappers[real_fidx].release());
      if (cur_bin_mappers.back()->GetDefaultBin() != cur_bin_mappers.back()->GetMostFreqBin()) {
        feature_need_push_zeros_.push_back(cur_fidx);
      }
      ++cur_fidx;
    }
    feature_groups_.emplace_back(std::unique_ptr<FeatureGroup>(
      new FeatureGroup(cur_cnt_features, group_is_multi_val[i], &cur_bin_mappers, num_data_)));
  }
  Log::Info("Total groups %d, multi-val groups %d.", num_groups_, num_multi_val_group);
  feature_groups_.shrink_to_fit();
  group_bin_boundaries_.clear();
  uint64_t num_total_bin = 0;
  group_bin_boundaries_.push_back(num_total_bin);
  for (int i = 0; i < num_groups_; ++i) {
    num_total_bin += feature_groups_[i]->num_total_bin_;
    group_bin_boundaries_.push_back(num_total_bin);
  }
  int last_group = 0;
  group_feature_start_.reserve(num_groups_);
  group_feature_cnt_.reserve(num_groups_);
  group_feature_start_.push_back(0);
  group_feature_cnt_.push_back(1);
  for (int i = 1; i < num_features_; ++i) {
    const int group = feature2group_[i];
    if (group == last_group) {
      group_feature_cnt_.back() = group_feature_cnt_.back() + 1;
    } else {
      group_feature_start_.push_back(i);
      group_feature_cnt_.push_back(1);
      last_group = group;
    }
  }

  if (!io_config.monotone_constraints.empty()) {
    CHECK(static_cast<size_t>(num_total_features_) == io_config.monotone_constraints.size());
    monotone_types_.resize(num_features_);
    for (int i = 0; i < num_total_features_; ++i) {
      int inner_fidx = InnerFeatureIndex(i);
      if (inner_fidx >= 0) {
        monotone_types_[inner_fidx] = io_config.monotone_constraints[i];
      }
    }
    if (ArrayArgs<int8_t>::CheckAllZero(monotone_types_)) {
      monotone_types_.clear();
    }
  }
  if (!io_config.feature_contri.empty()) {
    CHECK(static_cast<size_t>(num_total_features_) == io_config.feature_contri.size());
    feature_penalty_.resize(num_features_);
    for (int i = 0; i < num_total_features_; ++i) {
      int inner_fidx = InnerFeatureIndex(i);
      if (inner_fidx >= 0) {
        feature_penalty_[inner_fidx] = std::max(0.0, io_config.feature_contri[i]);
      }
    }
    if (ArrayArgs<double>::CheckAll(feature_penalty_, 1.0)) {
      feature_penalty_.clear();
    }
  }
  if (!io_config.max_bin_by_feature.empty()) {
    CHECK(static_cast<size_t>(num_total_features_) == io_config.max_bin_by_feature.size());
    CHECK(*(std::min_element(io_config.max_bin_by_feature.begin(), io_config.max_bin_by_feature.end())) > 1);
    max_bin_by_feature_.resize(num_total_features_);
    max_bin_by_feature_.assign(io_config.max_bin_by_feature.begin(), io_config.max_bin_by_feature.end());
  }
  forced_bin_bounds_ = forced_bins;
  max_bin_ = io_config.max_bin;
  min_data_in_bin_ = io_config.min_data_in_bin;
  bin_construct_sample_cnt_ = io_config.bin_construct_sample_cnt;
  use_missing_ = io_config.use_missing;
  zero_as_missing_ = io_config.zero_as_missing;
}

void Dataset::ResetConfig(const char* parameters) {
  auto param = Config::Str2Map(parameters);
  Config io_config;
  io_config.Set(param);
  if (param.count("max_bin") && io_config.max_bin != max_bin_) {
    Log::Warning("Cannot change max_bin after constructed Dataset handle.");
  }
  if (param.count("max_bin_by_feature") && io_config.max_bin_by_feature != max_bin_by_feature_) {
    Log::Warning("Cannot change max_bin_by_feature after constructed Dataset handle.");
  }
  if (param.count("bin_construct_sample_cnt") && io_config.bin_construct_sample_cnt != bin_construct_sample_cnt_) {
    Log::Warning("Cannot change bin_construct_sample_cnt after constructed Dataset handle.");
  }
  if (param.count("min_data_in_bin") && io_config.min_data_in_bin != min_data_in_bin_) {
    Log::Warning("Cannot change min_data_in_bin after constructed Dataset handle.");
  }
  if (param.count("use_missing") && io_config.use_missing != use_missing_) {
    Log::Warning("Cannot change use_missing after constructed Dataset handle.");
  }
  if (param.count("zero_as_missing") && io_config.zero_as_missing != zero_as_missing_) {
    Log::Warning("Cannot change zero_as_missing after constructed Dataset handle.");
  }
  if (param.count("forcedbins_filename")) {
    Log::Warning("Cannot change forced bins after constructed Dataset handle.");
  }

  if (!io_config.monotone_constraints.empty()) {
    CHECK(static_cast<size_t>(num_total_features_) == io_config.monotone_constraints.size());
    monotone_types_.resize(num_features_);
    for (int i = 0; i < num_total_features_; ++i) {
      int inner_fidx = InnerFeatureIndex(i);
      if (inner_fidx >= 0) {
        monotone_types_[inner_fidx] = io_config.monotone_constraints[i];
      }
    }
    if (ArrayArgs<int8_t>::CheckAllZero(monotone_types_)) {
      monotone_types_.clear();
    }
  }
  if (!io_config.feature_contri.empty()) {
    CHECK(static_cast<size_t>(num_total_features_) == io_config.feature_contri.size());
    feature_penalty_.resize(num_features_);
    for (int i = 0; i < num_total_features_; ++i) {
      int inner_fidx = InnerFeatureIndex(i);
      if (inner_fidx >= 0) {
        feature_penalty_[inner_fidx] = std::max(0.0, io_config.feature_contri[i]);
      }
    }
    if (ArrayArgs<double>::CheckAll(feature_penalty_, 1.0)) {
      feature_penalty_.clear();
    }
  }
}

void Dataset::FinishLoad() {
  if (is_finish_load_) { return; }
  if (num_groups_ > 0) {
    OMP_INIT_EX();
#pragma omp parallel for schedule(guided)
    for (int i = 0; i < num_groups_; ++i) {
      OMP_LOOP_EX_BEGIN();
      feature_groups_[i]->bin_data_->FinishLoad();
      OMP_LOOP_EX_END();
    }
    OMP_THROW_EX();
  }
  is_finish_load_ = true;
}

void Dataset::CopyFeatureMapperFrom(const Dataset* dataset) {
  feature_groups_.clear();
  num_features_ = dataset->num_features_;
  num_groups_ = dataset->num_groups_;
  // copy feature bin mapper data
  for (int i = 0; i < num_groups_; ++i) {
    std::vector<std::unique_ptr<BinMapper>> bin_mappers;
    for (int j = 0; j < dataset->feature_groups_[i]->num_feature_; ++j) {
      bin_mappers.emplace_back(new BinMapper(*(dataset->feature_groups_[i]->bin_mappers_[j])));
    }
    feature_groups_.emplace_back(new FeatureGroup(
      dataset->feature_groups_[i]->num_feature_,
      dataset->feature_groups_[i]->is_multi_val_,
      &bin_mappers,
      num_data_));
  }
  feature_groups_.shrink_to_fit();
  used_feature_map_ = dataset->used_feature_map_;
  num_total_features_ = dataset->num_total_features_;
  feature_names_ = dataset->feature_names_;
  label_idx_ = dataset->label_idx_;
  real_feature_idx_ = dataset->real_feature_idx_;
  feature2group_ = dataset->feature2group_;
  feature2subfeature_ = dataset->feature2subfeature_;
  group_bin_boundaries_ = dataset->group_bin_boundaries_;
  group_feature_start_ = dataset->group_feature_start_;
  group_feature_cnt_ = dataset->group_feature_cnt_;
  monotone_types_ = dataset->monotone_types_;
  feature_penalty_ = dataset->feature_penalty_;
  forced_bin_bounds_ = dataset->forced_bin_bounds_;
  feature_need_push_zeros_ = dataset->feature_need_push_zeros_;
}

void Dataset::CreateValid(const Dataset* dataset) {
  feature_groups_.clear();
  num_features_ = dataset->num_features_;
  num_groups_ = num_features_;
  feature2group_.clear();
  feature2subfeature_.clear();
  // copy feature bin mapper data
  feature_need_push_zeros_.clear();
  for (int i = 0; i < num_features_; ++i) {
    std::vector<std::unique_ptr<BinMapper>> bin_mappers;
    bin_mappers.emplace_back(new BinMapper(*(dataset->FeatureBinMapper(i))));
    if (bin_mappers.back()->GetDefaultBin() != bin_mappers.back()->GetMostFreqBin()) {
      feature_need_push_zeros_.push_back(i);
    }
    bool is_sparse = bin_mappers[0]->sparse_rate() > 0.8 ? true : false;
    feature_groups_.emplace_back(new FeatureGroup(&bin_mappers,
                                                  num_data_,
                                                  is_sparse));
    feature2group_.push_back(i);
    feature2subfeature_.push_back(0);
  }

  feature_groups_.shrink_to_fit();
  used_feature_map_ = dataset->used_feature_map_;
  num_total_features_ = dataset->num_total_features_;
  feature_names_ = dataset->feature_names_;
  label_idx_ = dataset->label_idx_;
  real_feature_idx_ = dataset->real_feature_idx_;
  group_bin_boundaries_.clear();
  uint64_t num_total_bin = 0;
  group_bin_boundaries_.push_back(num_total_bin);
  for (int i = 0; i < num_groups_; ++i) {
    num_total_bin += feature_groups_[i]->num_total_bin_;
    group_bin_boundaries_.push_back(num_total_bin);
  }
  int last_group = 0;
  group_feature_start_.reserve(num_groups_);
  group_feature_cnt_.reserve(num_groups_);
  group_feature_start_.push_back(0);
  group_feature_cnt_.push_back(1);
  for (int i = 1; i < num_features_; ++i) {
    const int group = feature2group_[i];
    if (group == last_group) {
      group_feature_cnt_.back() = group_feature_cnt_.back() + 1;
    } else {
      group_feature_start_.push_back(i);
      group_feature_cnt_.push_back(1);
      last_group = group;
    }
  }
  monotone_types_ = dataset->monotone_types_;
  feature_penalty_ = dataset->feature_penalty_;
  forced_bin_bounds_ = dataset->forced_bin_bounds_;
}

void Dataset::ReSize(data_size_t num_data) {
  if (num_data_ != num_data) {
    num_data_ = num_data;
    OMP_INIT_EX();
    #pragma omp parallel for schedule(static)
    for (int group = 0; group < num_groups_; ++group) {
      OMP_LOOP_EX_BEGIN();
      feature_groups_[group]->bin_data_->ReSize(num_data_);
      OMP_LOOP_EX_END();
    }
    OMP_THROW_EX();
  }
}

void Dataset::CopySubset(const Dataset* fullset, const data_size_t* used_indices, data_size_t num_used_indices, bool need_meta_data) {
  CHECK(num_used_indices == num_data_);
  OMP_INIT_EX();
  #pragma omp parallel for schedule(static)
  for (int group = 0; group < num_groups_; ++group) {
    OMP_LOOP_EX_BEGIN();
    feature_groups_[group]->CopySubset(fullset->feature_groups_[group].get(), used_indices, num_used_indices);
    OMP_LOOP_EX_END();
  }
  OMP_THROW_EX();
  if (need_meta_data) {
    metadata_.Init(fullset->metadata_, used_indices, num_used_indices);
  }
  is_finish_load_ = true;
}

bool Dataset::SetFloatField(const char* field_name, const float* field_data, data_size_t num_element) {
  std::string name(field_name);
  name = Common::Trim(name);
  if (name == std::string("label") || name == std::string("target")) {
    #ifdef LABEL_T_USE_DOUBLE
    Log::Fatal("Don't support LABEL_T_USE_DOUBLE");
    #else
    metadata_.SetLabel(field_data, num_element);
    #endif
  } else if (name == std::string("weight") || name == std::string("weights")) {
    #ifdef LABEL_T_USE_DOUBLE
    Log::Fatal("Don't support LABEL_T_USE_DOUBLE");
    #else
    metadata_.SetWeights(field_data, num_element);
    #endif
  } else {
    return false;
  }
  return true;
}

bool Dataset::SetDoubleField(const char* field_name, const double* field_data, data_size_t num_element) {
  std::string name(field_name);
  name = Common::Trim(name);
  if (name == std::string("init_score")) {
    metadata_.SetInitScore(field_data, num_element);
  } else {
    return false;
  }
  return true;
}

bool Dataset::SetIntField(const char* field_name, const int* field_data, data_size_t num_element) {
  std::string name(field_name);
  name = Common::Trim(name);
  if (name == std::string("query") || name == std::string("group")) {
    metadata_.SetQuery(field_data, num_element);
  } else {
    return false;
  }
  return true;
}

bool Dataset::GetFloatField(const char* field_name, data_size_t* out_len, const float** out_ptr) {
  std::string name(field_name);
  name = Common::Trim(name);
  if (name == std::string("label") || name == std::string("target")) {
    #ifdef LABEL_T_USE_DOUBLE
    Log::Fatal("Don't support LABEL_T_USE_DOUBLE");
    #else
    *out_ptr = metadata_.label();
    *out_len = num_data_;
    #endif
  } else if (name == std::string("weight") || name == std::string("weights")) {
    #ifdef LABEL_T_USE_DOUBLE
    Log::Fatal("Don't support LABEL_T_USE_DOUBLE");
    #else
    *out_ptr = metadata_.weights();
    *out_len = num_data_;
    #endif
  } else {
    return false;
  }
  return true;
}

bool Dataset::GetDoubleField(const char* field_name, data_size_t* out_len, const double** out_ptr) {
  std::string name(field_name);
  name = Common::Trim(name);
  if (name == std::string("init_score")) {
    *out_ptr = metadata_.init_score();
    *out_len = static_cast<data_size_t>(metadata_.num_init_score());
  } else if (name == std::string("feature_penalty")) {
    *out_ptr = feature_penalty_.data();
    *out_len = static_cast<data_size_t>(feature_penalty_.size());
  } else {
    return false;
  }
  return true;
}

bool Dataset::GetIntField(const char* field_name, data_size_t* out_len, const int** out_ptr) {
  std::string name(field_name);
  name = Common::Trim(name);
  if (name == std::string("query") || name == std::string("group")) {
    *out_ptr = metadata_.query_boundaries();
    *out_len = metadata_.num_queries() + 1;
  } else {
    return false;
  }
  return true;
}

bool Dataset::GetInt8Field(const char* field_name, data_size_t* out_len, const int8_t** out_ptr) {
  std::string name(field_name);
  name = Common::Trim(name);
  if (name == std::string("monotone_constraints")) {
    *out_ptr = monotone_types_.data();
    *out_len = static_cast<data_size_t>(monotone_types_.size());
  } else {
    return false;
  }
  return true;
}

void Dataset::SaveBinaryFile(const char* bin_filename) {
  if (bin_filename != nullptr
      && std::string(bin_filename) == data_filename_) {
    Log::Warning("Bianry file %s already exists", bin_filename);
    return;
  }
  // if not pass a filename, just append ".bin" of original file
  std::string bin_filename_str(data_filename_);
  if (bin_filename == nullptr || bin_filename[0] == '\0') {
    bin_filename_str.append(".bin");
    bin_filename = bin_filename_str.c_str();
  }
  bool is_file_existed = false;

  if (VirtualFileWriter::Exists(bin_filename)) {
    is_file_existed = true;
    Log::Warning("File %s exists, cannot save binary to it", bin_filename);
  }

  if (!is_file_existed) {
    auto writer = VirtualFileWriter::Make(bin_filename);
    if (!writer->Init()) {
      Log::Fatal("Cannot write binary data to %s ", bin_filename);
    }
    Log::Info("Saving data to binary file %s", bin_filename);
    size_t size_of_token = std::strlen(binary_file_token);
    writer->Write(binary_file_token, size_of_token);
    // get size of header
    size_t size_of_header = sizeof(num_data_) + sizeof(num_features_) + sizeof(num_total_features_)
      + sizeof(int) * num_total_features_ + sizeof(label_idx_) + sizeof(num_groups_)
      + 3 * sizeof(int) * num_features_ + sizeof(uint64_t) * (num_groups_ + 1) + 2 * sizeof(int) * num_groups_ + sizeof(int8_t) * num_features_
      + sizeof(double) * num_features_ + sizeof(int32_t) * num_total_features_ + sizeof(int) * 3 + sizeof(bool) * 2;
    // size of feature names
    for (int i = 0; i < num_total_features_; ++i) {
      size_of_header += feature_names_[i].size() + sizeof(int);
    }
    // size of forced bins
    for (int i = 0; i < num_total_features_; ++i) {
      size_of_header += forced_bin_bounds_[i].size() * sizeof(double) + sizeof(int);
    }
    writer->Write(&size_of_header, sizeof(size_of_header));
    // write header
    writer->Write(&num_data_, sizeof(num_data_));
    writer->Write(&num_features_, sizeof(num_features_));
    writer->Write(&num_total_features_, sizeof(num_total_features_));
    writer->Write(&label_idx_, sizeof(label_idx_));
    writer->Write(&max_bin_, sizeof(max_bin_));
    writer->Write(&bin_construct_sample_cnt_, sizeof(bin_construct_sample_cnt_));
    writer->Write(&min_data_in_bin_, sizeof(min_data_in_bin_));
    writer->Write(&use_missing_, sizeof(use_missing_));
    writer->Write(&zero_as_missing_, sizeof(zero_as_missing_));
    writer->Write(used_feature_map_.data(), sizeof(int) * num_total_features_);
    writer->Write(&num_groups_, sizeof(num_groups_));
    writer->Write(real_feature_idx_.data(), sizeof(int) * num_features_);
    writer->Write(feature2group_.data(), sizeof(int) * num_features_);
    writer->Write(feature2subfeature_.data(), sizeof(int) * num_features_);
    writer->Write(group_bin_boundaries_.data(), sizeof(uint64_t) * (num_groups_ + 1));
    writer->Write(group_feature_start_.data(), sizeof(int) * num_groups_);
    writer->Write(group_feature_cnt_.data(), sizeof(int) * num_groups_);
    if (monotone_types_.empty()) {
      ArrayArgs<int8_t>::Assign(&monotone_types_, 0, num_features_);
    }
    writer->Write(monotone_types_.data(), sizeof(int8_t) * num_features_);
    if (ArrayArgs<int8_t>::CheckAllZero(monotone_types_)) {
      monotone_types_.clear();
    }
    if (feature_penalty_.empty()) {
      ArrayArgs<double>::Assign(&feature_penalty_, 1.0, num_features_);
    }
    writer->Write(feature_penalty_.data(), sizeof(double) * num_features_);
    if (ArrayArgs<double>::CheckAll(feature_penalty_, 1.0)) {
      feature_penalty_.clear();
    }
    if (max_bin_by_feature_.empty()) {
      ArrayArgs<int32_t>::Assign(&max_bin_by_feature_, -1, num_total_features_);
    }
    writer->Write(max_bin_by_feature_.data(), sizeof(int32_t) * num_total_features_);
    if (ArrayArgs<int32_t>::CheckAll(max_bin_by_feature_, -1)) {
      max_bin_by_feature_.clear();
    }
    // write feature names
    for (int i = 0; i < num_total_features_; ++i) {
      int str_len = static_cast<int>(feature_names_[i].size());
      writer->Write(&str_len, sizeof(int));
      const char* c_str = feature_names_[i].c_str();
      writer->Write(c_str, sizeof(char) * str_len);
    }
    // write forced bins
    for (int i = 0; i < num_total_features_; ++i) {
      int num_bounds = static_cast<int>(forced_bin_bounds_[i].size());
      writer->Write(&num_bounds, sizeof(int));

      for (size_t j = 0; j < forced_bin_bounds_[i].size(); ++j) {
        writer->Write(&forced_bin_bounds_[i][j], sizeof(double));
      }
    }

    // get size of meta data
    size_t size_of_metadata = metadata_.SizesInByte();
    writer->Write(&size_of_metadata, sizeof(size_of_metadata));
    // write meta data
    metadata_.SaveBinaryToFile(writer.get());

    // write feature data
    for (int i = 0; i < num_groups_; ++i) {
      // get size of feature
      size_t size_of_feature = feature_groups_[i]->SizesInByte();
      writer->Write(&size_of_feature, sizeof(size_of_feature));
      // write feature
      feature_groups_[i]->SaveBinaryToFile(writer.get());
    }
  }
}

void Dataset::DumpTextFile(const char* text_filename) {
  FILE* file = NULL;
#if _MSC_VER
  fopen_s(&file, text_filename, "wt");
#else
  file = fopen(text_filename, "wt");
#endif
  fprintf(file, "num_features: %d\n", num_features_);
  fprintf(file, "num_total_features: %d\n", num_total_features_);
  fprintf(file, "num_groups: %d\n", num_groups_);
  fprintf(file, "num_data: %d\n", num_data_);
  fprintf(file, "feature_names: ");
  for (auto n : feature_names_) {
    fprintf(file, "%s, ", n.c_str());
  }
  fprintf(file, "\nmonotone_constraints: ");
  for (auto i : monotone_types_) {
    fprintf(file, "%d, ", i);
  }
  fprintf(file, "\nfeature_penalty: ");
  for (auto i : feature_penalty_) {
    fprintf(file, "%lf, ", i);
  }
  fprintf(file, "\nmax_bin_by_feature: ");
  for (auto i : max_bin_by_feature_) {
    fprintf(file, "%d, ", i);
  }
  fprintf(file, "\n");
  for (auto n : feature_names_) {
    fprintf(file, "%s, ", n.c_str());
  }
  fprintf(file, "\nforced_bins: ");
  for (int i = 0; i < num_total_features_; ++i) {
    fprintf(file, "\nfeature %d: ", i);
    for (size_t j = 0; j < forced_bin_bounds_[i].size(); ++j) {
      fprintf(file, "%lf, ", forced_bin_bounds_[i][j]);
    }
  }
  std::vector<std::unique_ptr<BinIterator>> iterators;
  iterators.reserve(num_features_);
  for (int j = 0; j < num_features_; ++j) {
    auto group_idx = feature2group_[j];
    auto sub_idx = feature2subfeature_[j];
    iterators.emplace_back(feature_groups_[group_idx]->SubFeatureIterator(sub_idx));
  }
  for (data_size_t i = 0; i < num_data_; ++i) {
    fprintf(file, "\n");
    for (int j = 0; j < num_total_features_; ++j) {
      auto inner_feature_idx = used_feature_map_[j];
      if (inner_feature_idx < 0) {
        fprintf(file, "NA, ");
      } else {
        fprintf(file, "%d, ", iterators[inner_feature_idx]->Get(i));
      }
    }
  }
  fclose(file);
}

void Dataset::ConstructHistograms(const std::vector<int8_t>& is_feature_used,
                                  const data_size_t* data_indices, data_size_t num_data,
                                  int leaf_idx,
                                  const score_t* gradients, const score_t* hessians,
                                  score_t* ordered_gradients, score_t* ordered_hessians,
                                  bool is_constant_hessian,
                                  hist_t* hist_data) const {
  if (leaf_idx < 0 || num_data < 0 || hist_data == nullptr) {
    return;
  }
  int num_threads = 1;
  #pragma omp parallel
  #pragma omp master
  {
    num_threads = omp_get_num_threads();
  }
  std::vector<int> used_dense_group;
  std::vector<int> used_sparse_group;
  used_dense_group.reserve(num_groups_);
  used_sparse_group.reserve(num_groups_);
  for (int group = 0; group < num_groups_; ++group) {
    const int f_cnt = group_feature_cnt_[group];
    bool is_group_used = false;
    for (int j = 0; j < f_cnt; ++j) {
      const int fidx = group_feature_start_[group] + j;
      if (is_feature_used[fidx]) {
        is_group_used = true;
        break;
      }
    }
    if (is_group_used) {
      if (feature_groups_[group]->is_multi_val_) {
        used_sparse_group.push_back(group);
      } else {
        used_dense_group.push_back(group);
      }
    }
  }
  int num_used_dense_group = static_cast<int>(used_dense_group.size());
  int num_used_sparse_group = static_cast<int>(used_sparse_group.size());

  auto ptr_ordered_grad = gradients;
  auto ptr_ordered_hess = hessians;
  #ifdef TIMETAG
  auto start_time = std::chrono::steady_clock::now();
  #endif
  if (data_indices != nullptr && num_data < num_data_) {
    if (!is_constant_hessian) {
      #pragma omp parallel for schedule(static)
      for (data_size_t i = 0; i < num_data; ++i) {
        ordered_gradients[i] = gradients[data_indices[i]];
        ordered_hessians[i] = hessians[data_indices[i]];
      }
    } else {
      #pragma omp parallel for schedule(static)
      for (data_size_t i = 0; i < num_data; ++i) {
        ordered_gradients[i] = gradients[data_indices[i]];
      }
    }
    ptr_ordered_grad = ordered_gradients;
    ptr_ordered_hess = ordered_hessians;
    if (!is_constant_hessian) {
      OMP_INIT_EX();
      #pragma omp parallel for schedule(static)
      for (int gi = 0; gi < num_used_dense_group; ++gi) {
        OMP_LOOP_EX_BEGIN();
        int group = used_dense_group[gi];
        // feature is not used
        auto data_ptr = hist_data + group_bin_boundaries_[group] * 2;
        const int num_bin = feature_groups_[group]->num_total_bin_;
        std::memset(reinterpret_cast<void*>(data_ptr), 0, num_bin* KHistEntrySize);
        // construct histograms for smaller leaf
        feature_groups_[group]->bin_data_->ConstructHistogram(
          data_indices,
          0,
          num_data,
          ptr_ordered_grad,
          ptr_ordered_hess,
          data_ptr);
        OMP_LOOP_EX_END();
      }
      OMP_THROW_EX();

    } else {
      OMP_INIT_EX();
      #pragma omp parallel for schedule(static)
      for (int gi = 0; gi < num_used_dense_group; ++gi) {
        OMP_LOOP_EX_BEGIN();
        int group = used_dense_group[gi];
        // feature is not used
        auto data_ptr = hist_data + group_bin_boundaries_[group] * 2;
        const int num_bin = feature_groups_[group]->num_total_bin_;
        std::memset(reinterpret_cast<void*>(data_ptr), 0, num_bin* KHistEntrySize);
        // construct histograms for smaller leaf
        feature_groups_[group]->bin_data_->ConstructHistogram(
          data_indices,
          0,
          num_data,
          ptr_ordered_grad,
          data_ptr);
        // fixed hessian.
        for (int i = 0; i < num_bin; ++i) {
          GET_HESS(data_ptr, i) = GET_HESS(data_ptr, i) * hessians[0];
        }
        OMP_LOOP_EX_END();
      }
      OMP_THROW_EX();
    }
  } else {
    if (!is_constant_hessian) {
      OMP_INIT_EX();
      #pragma omp parallel for schedule(static)
      for (int gi = 0; gi < num_used_dense_group; ++gi) {
        OMP_LOOP_EX_BEGIN();
        int group = used_dense_group[gi];
        // feature is not used
        auto data_ptr = hist_data + group_bin_boundaries_[group] * 2;
        const int num_bin = feature_groups_[group]->num_total_bin_;
        std::memset(reinterpret_cast<void*>(data_ptr), 0, num_bin* KHistEntrySize);
        // construct histograms for smaller leaf
        feature_groups_[group]->bin_data_->ConstructHistogram(
          0,
          num_data,
          ptr_ordered_grad,
          ptr_ordered_hess,
          data_ptr);
        OMP_LOOP_EX_END();
      }
      OMP_THROW_EX();
    } else {
      OMP_INIT_EX();
      #pragma omp parallel for schedule(static)
      for (int gi = 0; gi < num_used_dense_group; ++gi) {
        OMP_LOOP_EX_BEGIN();
        int group = used_dense_group[gi];
        // feature is not used
        auto data_ptr = hist_data + group_bin_boundaries_[group] * 2;
        const int num_bin = feature_groups_[group]->num_total_bin_;
        std::memset(reinterpret_cast<void*>(data_ptr), 0, num_bin * KHistEntrySize);
        // construct histograms for smaller leaf
        feature_groups_[group]->bin_data_->ConstructHistogram(
          0,
          num_data,
          ptr_ordered_grad,
          data_ptr);
        // fixed hessian.
        for (int i = 0; i < num_bin; ++i) {
          GET_HESS(data_ptr, i) = GET_HESS(data_ptr, i) * hessians[0];
        }
        OMP_LOOP_EX_END();
      }
      OMP_THROW_EX();
    }
  }
  #ifdef TIMETAG
  dense_bin_time  += std::chrono::steady_clock::now() - start_time;
  #endif
  // for sparse bin
  if (num_used_sparse_group > 0) {
    for (int gi = 0; gi < num_used_sparse_group; ++gi) {
      #ifdef TIMETAG
      start_time = std::chrono::steady_clock::now();
      #endif
      int group = used_sparse_group[gi];
      const int num_bin = feature_groups_[group]->num_total_bin_;
      if (2 * num_bin * num_threads > static_cast<int>(hist_buf_.size())) {
        hist_buf_.resize(2 * num_bin * num_threads);
        Log::Info("number of buffered bin %d", num_bin);
      }
      #ifdef TIMETAG
      sparse_hist_prep_time += std::chrono::steady_clock::now() - start_time;
      start_time = std::chrono::steady_clock::now();
      #endif
      const int min_row_size = 512;
      const int n_part = std::min(num_threads, (num_data + min_row_size - 1) / min_row_size);
      const int step = (num_data + n_part - 1) / n_part;
      #pragma omp parallel for schedule(static)
      for (int tid = 0; tid < n_part; ++tid) {
        data_size_t start = tid * step;
        data_size_t end = std::min(start + step, num_data);
        auto data_ptr = hist_buf_.data() + tid * num_bin * 2;
        std::memset(reinterpret_cast<void*>(data_ptr), 0, num_bin* KHistEntrySize);
        if (data_indices != nullptr && num_data < num_data_) {
          if (!is_constant_hessian) {
            feature_groups_[group]->bin_data_->ConstructHistogram(
              data_indices,
              start,
              end,
              ptr_ordered_grad,
              ptr_ordered_hess,
              data_ptr);
          } else {
            feature_groups_[group]->bin_data_->ConstructHistogram(
              data_indices,
              start,
              end,
              ptr_ordered_grad,
              data_ptr);
          }
        } else {
          if (!is_constant_hessian) {
            feature_groups_[group]->bin_data_->ConstructHistogram(
              start,
              end,
              ptr_ordered_grad,
              ptr_ordered_hess,
              data_ptr);
          } else {
            feature_groups_[group]->bin_data_->ConstructHistogram(
              start,
              end,
              ptr_ordered_grad,
              data_ptr);
          }
        }
      }
      #ifdef TIMETAG
      sparse_bin_time += std::chrono::steady_clock::now() - start_time;
      start_time = std::chrono::steady_clock::now();
      #endif
      auto data_ptr = hist_data + group_bin_boundaries_[group] * 2;
      std::memset(reinterpret_cast<void*>(data_ptr), 0, num_bin * KHistEntrySize);

      // don't merge bin 0
      const int min_block_size = 512;
      const int n_block = std::min(num_threads, (num_bin + min_block_size - 2) / min_block_size);
      const int num_bin_per_threads = (num_bin + n_block - 2) / n_block;
      if (!is_constant_hessian) {
        #pragma omp parallel for schedule(static)
        for (int t = 0; t < n_block; ++t) {
          const int start = t * num_bin_per_threads + 1;
          const int end = std::min(start + num_bin_per_threads, num_bin);
          for (int tid = 0; tid < n_part; ++tid) {
            auto src_ptr = hist_buf_.data() + tid * num_bin * 2;
            for (int i = start; i < end; i++) {
              GET_GRAD(data_ptr, i) += GET_GRAD(src_ptr, i);
              GET_HESS(data_ptr, i) += GET_HESS(src_ptr, i);
            }
          }
        }
      } else {
        #pragma omp parallel for schedule(static)
        for (int t = 0; t < n_block; ++t) {
          const int start = t * num_bin_per_threads + 1;
          const int end = std::min(start + num_bin_per_threads, num_bin);
          for (int tid = 0; tid < n_part; ++tid) {
            auto src_ptr = hist_buf_.data() + tid * num_bin * 2;
            for (int i = start; i < end; i++) {
              GET_GRAD(data_ptr, i) += GET_GRAD(src_ptr, i);
              GET_HESS(data_ptr, i) += GET_HESS(src_ptr, i);
            }
          }
          for (int i = start; i < end; i++) {
            GET_HESS(data_ptr, i) = GET_HESS(data_ptr, i) * hessians[0];
          }
        }
      }
      #ifdef TIMETAG
      sparse_hist_merge_time += std::chrono::steady_clock::now() - start_time;
      #endif
    }
  }
}

void Dataset::FixHistogram(int feature_idx, double sum_gradient, double sum_hessian, data_size_t num_data,
                           hist_t* data) const {
  const int group = feature2group_[feature_idx];
  const int sub_feature = feature2subfeature_[feature_idx];
  const BinMapper* bin_mapper = feature_groups_[group]->bin_mappers_[sub_feature].get();
  const int most_freq_bin = bin_mapper->GetMostFreqBin();
  if (most_freq_bin > 0) {
    const int num_bin = bin_mapper->num_bin();
    GET_GRAD(data, most_freq_bin) = sum_gradient;
    GET_HESS(data, most_freq_bin) = sum_hessian;
    for (int i = 0; i < num_bin; ++i) {
      if (i != most_freq_bin) {
        GET_GRAD(data, most_freq_bin) -= GET_GRAD(data, i);
        GET_HESS(data, most_freq_bin) -= GET_HESS(data, i);
      }
    }
  }
}

template<typename T>
void PushVector(std::vector<T>* dest, const std::vector<T>& src) {
  dest->reserve(dest->size() + src.size());
  for (auto i : src) {
    dest->push_back(i);
  }
}

template<typename T>
void PushOffset(std::vector<T>* dest, const std::vector<T>& src, const T& offset) {
  dest->reserve(dest->size() + src.size());
  for (auto i : src) {
    dest->push_back(i + offset);
  }
}

template<typename T>
void PushClearIfEmpty(std::vector<T>* dest, const size_t dest_len, const std::vector<T>& src, const size_t src_len, const T& deflt) {
  if (!dest->empty() && !src.empty()) {
    PushVector(dest, src);
  } else if (!dest->empty() && src.empty()) {
    for (size_t i = 0; i < src_len; ++i) {
      dest->push_back(deflt);
    }
  } else if (dest->empty() && !src.empty()) {
    for (size_t i = 0; i < dest_len; ++i) {
      dest->push_back(deflt);
    }
    PushVector(dest, src);
  }
}

void Dataset::addFeaturesFrom(Dataset* other) {
  if (other->num_data_ != num_data_) {
    throw std::runtime_error("Cannot add features from other Dataset with a different number of rows");
  }
  PushVector(&feature_names_, other->feature_names_);
  PushVector(&feature2subfeature_, other->feature2subfeature_);
  PushVector(&group_feature_cnt_, other->group_feature_cnt_);
  PushVector(&forced_bin_bounds_, other->forced_bin_bounds_);
  feature_groups_.reserve(other->feature_groups_.size());
  for (auto& fg : other->feature_groups_) {
    feature_groups_.emplace_back(new FeatureGroup(*fg));
  }
  for (auto feature_idx : other->used_feature_map_) {
    if (feature_idx >= 0) {
      used_feature_map_.push_back(feature_idx + num_features_);
    } else {
      used_feature_map_.push_back(-1);  // Unused feature.
    }
  }
  PushOffset(&real_feature_idx_, other->real_feature_idx_, num_total_features_);
  PushOffset(&feature2group_, other->feature2group_, num_groups_);
  auto bin_offset = group_bin_boundaries_.back();
  // Skip the leading 0 when copying group_bin_boundaries.
  for (auto i = other->group_bin_boundaries_.begin()+1; i < other->group_bin_boundaries_.end(); ++i) {
    group_bin_boundaries_.push_back(*i + bin_offset);
  }
  PushOffset(&group_feature_start_, other->group_feature_start_, num_features_);

  PushClearIfEmpty(&monotone_types_, num_total_features_, other->monotone_types_, other->num_total_features_, (int8_t)0);
  PushClearIfEmpty(&feature_penalty_, num_total_features_, other->feature_penalty_, other->num_total_features_, 1.0);
  PushClearIfEmpty(&max_bin_by_feature_, num_total_features_, other->max_bin_by_feature_, other->num_total_features_, -1);

  num_features_ += other->num_features_;
  num_total_features_ += other->num_total_features_;
  num_groups_ += other->num_groups_;
}

}  // namespace LightGBM
