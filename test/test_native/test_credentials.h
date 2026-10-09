#pragma once
// Tests for the credential architecture (src/ct_credentials.h).
// Included by test_main.cpp; runCredentialTests() is called from main().
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "ct_credentials.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Fixtures
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static uint32_t s_credRndState = 12345u;
static int      s_credRndCalls = 0;

static uint32_t credCountingRnd(void) {
    ++s_credRndCalls;
    s_credRndState = s_credRndState * 1664525u + 1013904223u;
    return s_credRndState;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Tests
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static void test_credentials_personal_mode_is_the_fixed_default(void) {
    CtCredentials c;
    s_credRndCalls = 0;
    TEST_ASSERT_TRUE(ctProvisionInitialCredentials(&c, 0, credCountingRnd));
    TEST_ASSERT_EQUAL_STRING("CarTouch", c.user);
    TEST_ASSERT_EQUAL_STRING("12345678", c.webPass);
    // Wi-Fi key and web password are the same fixed value in personal mode.
    TEST_ASSERT_EQUAL_STRING(c.webPass, c.apKey);
    TEST_ASSERT_FALSE(c.mustChange);
    // The generator must never run in personal mode.
    TEST_ASSERT_EQUAL_INT(0, s_credRndCalls);
    // A second device gets exactly the same login.
    CtCredentials d;
    TEST_ASSERT_TRUE(ctProvisionInitialCredentials(&d, 0, nullptr));
    TEST_ASSERT_EQUAL_STRING(c.webPass, d.webPass);
}

static void test_credentials_commercial_mode_is_unique_and_valid(void) {
    CtCredentials a, b;
    TEST_ASSERT_TRUE(ctProvisionInitialCredentials(&a, 1, credCountingRnd));
    TEST_ASSERT_TRUE(ctProvisionInitialCredentials(&b, 1, credCountingRnd));
    TEST_ASSERT_EQUAL_UINT32(CT_GEN_PASS_LEN, strlen(a.webPass));
    TEST_ASSERT_EQUAL_UINT32(CT_GEN_PASS_LEN, strlen(a.apKey));
    TEST_ASSERT_TRUE(strlen(a.webPass) >= 8 && strlen(a.webPass) <= 15);
    TEST_ASSERT_TRUE(strcmp(a.webPass, b.webPass) != 0);
    TEST_ASSERT_TRUE(strcmp(a.webPass, a.apKey) != 0);  // separate keys
    TEST_ASSERT_TRUE(strcmp(a.webPass, WEB_DEFAULT_PASS) != 0);
    TEST_ASSERT_TRUE(a.mustChange);
    TEST_ASSERT_TRUE(s_credRndCalls > 0);
}

static void test_credentials_commercial_mode_never_falls_back_to_default(void) {
    CtCredentials c;
    TEST_ASSERT_FALSE(ctProvisionInitialCredentials(&c, 1, nullptr));
    TEST_ASSERT_TRUE(strcmp(c.webPass, WEB_DEFAULT_PASS) != 0);
    TEST_ASSERT_FALSE(ctProvisionInitialCredentials(nullptr, 0, nullptr));
}

static void test_credentials_ap_key_follows_web_or_is_separate(void) {
    // Personal: follows the web password, also after the owner changed it.
    TEST_ASSERT_EQUAL_STRING("newpass99", ctEffectiveApKey(false, "newpass99", ""));
    // Commercial: its own value, independent of the web password.
    TEST_ASSERT_EQUAL_STRING("apkey1234", ctEffectiveApKey(true, "webpass99", "apkey1234"));
    // Too short or missing: factory key, never empty (that would be an open AP).
    TEST_ASSERT_EQUAL_STRING(CT_AP_DEFAULT_PASS, ctEffectiveApKey(false, "short", ""));
    TEST_ASSERT_EQUAL_STRING(CT_AP_DEFAULT_PASS, ctEffectiveApKey(true, "webpass99", ""));
    TEST_ASSERT_EQUAL_STRING(CT_AP_DEFAULT_PASS, ctEffectiveApKey(true, "webpass99", nullptr));
}

static void test_credentials_password_change_pending(void) {
    // Personal mode (CT_REQUIRE_PASSWORD_CHANGE=0): never pending.
    TEST_ASSERT_FALSE(ctPasswordChangePending(false, false, WEB_DEFAULT_PASS));
    TEST_ASSERT_FALSE(ctPasswordChangePending(false, true, "whatever1"));
    // Required: pending while default or while the factory flag is set.
    TEST_ASSERT_TRUE(ctPasswordChangePending(true, false, WEB_DEFAULT_PASS));
    TEST_ASSERT_TRUE(ctPasswordChangePending(true, true, "Uniq3Passw0"));
    TEST_ASSERT_FALSE(ctPasswordChangePending(true, false, "ownerpass1"));
}

static void test_credentials_build_defaults_match_personal_behaviour(void) {
    TEST_ASSERT_EQUAL_INT(0, CT_PRODUCT_MODE);
    TEST_ASSERT_EQUAL_INT(0, CT_REQUIRE_PASSWORD_CHANGE);
    TEST_ASSERT_EQUAL_INT(0, CT_AP_KEY_SEPARATE);
    TEST_ASSERT_EQUAL_STRING("CarTouch", WEB_DEFAULT_USER);
    TEST_ASSERT_EQUAL_STRING("12345678", WEB_DEFAULT_PASS);
    TEST_ASSERT_EQUAL_STRING(WEB_DEFAULT_PASS, CT_AP_DEFAULT_PASS);
}

static void test_ct_copy_str_always_terminates(void) {
    char buf[5];
    ctCopyStr(buf, sizeof(buf), "abcdefgh");
    TEST_ASSERT_EQUAL_STRING("abcd", buf);
    ctCopyStr(buf, sizeof(buf), nullptr);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Registration
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static void runCredentialTests(void) {
    RUN_TEST(test_credentials_personal_mode_is_the_fixed_default);
    RUN_TEST(test_credentials_commercial_mode_is_unique_and_valid);
    RUN_TEST(test_credentials_commercial_mode_never_falls_back_to_default);
    RUN_TEST(test_credentials_ap_key_follows_web_or_is_separate);
    RUN_TEST(test_credentials_password_change_pending);
    RUN_TEST(test_credentials_build_defaults_match_personal_behaviour);
    RUN_TEST(test_ct_copy_str_always_terminates);
}
