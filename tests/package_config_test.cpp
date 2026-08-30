// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Unit tests for the shared package_config / class_c_config management core.
// Covers describe, get/set generation, dry-run, aliases, READ_ONLY, bounds.
// Behaviour contract for multi-vendor authors — keep green when changing the core.

#include "ocudu_dapp_sdk/package_config.h"
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ocudu::dapp::package_config;
using ocudu::dapp::reference::instance;

namespace {

struct scratch {
  double value{1.0};
  bool   dirty{false};
  bool   reject_validate{false};
};

double g_live = 1.0;

void begin_update(void*, void* s) noexcept
{
  auto* u  = static_cast<scratch*>(s);
  u->value = g_live;
  u->dirty = false;
}

ocudu_dapp_status_v1 validate(void*, void* s) noexcept
{
  auto* u = static_cast<scratch*>(s);
  if (u->reject_validate) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  return (u->value > 0.0 && u->value <= 10.0) ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
}

ocudu_dapp_status_v1 commit(void*, void* s) noexcept
{
  auto* u = static_cast<scratch*>(s);
  if (u->dirty) {
    g_live = u->value;
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 get_gain(void*, ocudu_dapp_value_v1* out) noexcept
{
  *out = value_double(g_live);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 stage_gain(void*, const ocudu_dapp_value_v1* in, void* s) noexcept
{
  auto* u = static_cast<scratch*>(s);
  if (!parse_double(*in, u->value)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  u->dirty = true;
  return OCUDU_DAPP_OK_V1;
}

const char* const kGainAliases[] = {"scale", nullptr};

const config_key kKeys[] = {
    instance_enabled_key(),
    double_key("gain", "Test gain", 1.0, 0.0, 10.0, get_gain, stage_gain, 0, kGainAliases),
    string_key(
        "ro_status", "Read-only",
        [](void*, ocudu_dapp_value_v1* out) noexcept -> ocudu_dapp_status_v1 {
          static const char kMsg[] = "ok";
          *out                     = value_string(kMsg);
          return OCUDU_DAPP_OK_V1;
        },
        nullptr, OCUDU_DAPP_CONFIG_READ_ONLY_V1),
};

const config_schema kSchema{kKeys, 3, sizeof(scratch), begin_update, validate, commit};

class PackageConfigTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    g_live = 1.0;
    // instance is not assignable (atomics / mutex); re-init fields in place.
    state.config_generation = 1;
    state.enabled.store(true);
    state.active.store(false);
    api = &make_config_management(kSchema);
  }

  instance                        state{};
  const ocudu_dapp_management_v1* api{nullptr};
};

} // namespace

TEST_F(PackageConfigTest, describe_lists_keys_and_enabled)
{
  uint32_t count = 0;
  ASSERT_EQ(api->describe_config(&state, nullptr, 0, &count), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(count, 3u);
  std::vector<ocudu_dapp_config_descriptor_v1> desc(count);
  ASSERT_EQ(api->describe_config(&state, desc.data(), count, &count), OCUDU_DAPP_OK_V1);
  EXPECT_STREQ(desc[0].key, "enabled");
  EXPECT_STREQ(desc[1].key, "gain");
  EXPECT_EQ(desc[2].flags & OCUDU_DAPP_CONFIG_READ_ONLY_V1, OCUDU_DAPP_CONFIG_READ_ONLY_V1);
}

TEST_F(PackageConfigTest, get_and_set_generation_and_dry_run)
{
  uint32_t                   count = 0;
  uint64_t                   gen   = 0;
  ocudu_dapp_config_entry_v1 entry{};
  ASSERT_EQ(api->get_config(&state, &entry, 1, &count, &gen), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(gen, 1u);
  EXPECT_EQ(count, 3u);
  EXPECT_STREQ(entry.key, "enabled");
  EXPECT_TRUE(entry.value.value.boolean != 0);

  ocudu_dapp_config_entry_v1 update_entry{};
  update_entry.key   = "gain";
  update_entry.value = value_double(2.5);
  ocudu_dapp_config_update_v1 update{};
  update.struct_size         = sizeof(update);
  update.nof_entries         = 1;
  update.entries             = &update_entry;
  update.expected_generation = 1;
  update.dry_run             = 1;
  uint64_t out_gen           = 0;
  ASSERT_EQ(api->set_config(&state, &update, &out_gen), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(out_gen, 1u);
  EXPECT_DOUBLE_EQ(g_live, 1.0); // dry-run does not commit

  update.dry_run = 0;
  ASSERT_EQ(api->set_config(&state, &update, &out_gen), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(out_gen, 2u);
  EXPECT_DOUBLE_EQ(g_live, 2.5);

  update.expected_generation = 1; // stale
  EXPECT_EQ(api->set_config(&state, &update, &out_gen), OCUDU_DAPP_CONFLICT_V1);
  EXPECT_EQ(out_gen, 2u);
}

TEST_F(PackageConfigTest, alias_and_read_only_and_bounds)
{
  ocudu_dapp_config_entry_v1 entry{};
  entry.key   = "scale"; // alias of gain
  entry.value = value_double(3.0);
  ocudu_dapp_config_update_v1 update{};
  update.struct_size         = sizeof(update);
  update.nof_entries         = 1;
  update.entries             = &entry;
  update.expected_generation = 1;
  uint64_t gen               = 0;
  ASSERT_EQ(api->set_config(&state, &update, &gen), OCUDU_DAPP_OK_V1);
  EXPECT_DOUBLE_EQ(g_live, 3.0);

  entry.key                  = "ro_status";
  entry.value                = value_string("nope");
  update.expected_generation = gen;
  EXPECT_EQ(api->set_config(&state, &update, &gen), OCUDU_DAPP_INVALID_INPUT_V1);

  entry.key                  = "gain";
  entry.value                = value_double(99.0); // out of bounds via validate
  update.expected_generation = gen;
  EXPECT_EQ(api->set_config(&state, &update, &gen), OCUDU_DAPP_INVALID_INPUT_V1);

  entry.key   = "unknown";
  entry.value = value_double(1.0);
  EXPECT_EQ(api->set_config(&state, &update, &gen), OCUDU_DAPP_INVALID_INPUT_V1);
}

TEST_F(PackageConfigTest, value_helpers)
{
  bool   b = false;
  double d = 0;
  EXPECT_TRUE(parse_bool(value_bool(true), b));
  EXPECT_TRUE(b);
  EXPECT_TRUE(parse_double(value_double(1.25), d));
  EXPECT_DOUBLE_EQ(d, 1.25);
  ocudu_dapp_value_v1 u{};
  u.kind               = OCUDU_DAPP_VALUE_UINT64_V1;
  u.value.uint64_value = 7;
  EXPECT_TRUE(parse_double(u, d));
  EXPECT_DOUBLE_EQ(d, 7.0);
  char buf[8];
  EXPECT_TRUE(parse_string(value_string("hi"), buf, sizeof(buf)));
  EXPECT_STREQ(buf, "hi");
}
