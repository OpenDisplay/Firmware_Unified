/* sec2_ctr_test.c -- counter exhaustion, with OD_SEC2_CTR_MAX lowered to 5 for this executable
 * (CMakeLists compiles od_sec2.c into it with the override). Shares sec2_test.c's harness. */
#define SEC2_CTR_SUITE
#pragma GCC diagnostic ignored "-Wunused-function"   /* sec2_test.c's cases, not run here */
#include "sec2_test.c"

int main(void)
{
    const uint8_t ok[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0x00u };
    const uint8_t fe[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0xFEu };
    unsigned i;

    CASE("the device seals counters 1..CTR_MAX, the last reply goes out, then teardown");
    setup(true);
    CHECK(OD_SEC2_CTR_MAX == 5u);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    for (i = 1u; i <= OD_SEC2_CTR_MAX; ++i) {
        CHECK(send_sealed(CMD_REBOOT, NULL, 0u) == 1u && reply_is(ok, 3u));
        CHECK(C.rx_ctr == i);
    }
    CHECK(!od_sec2_session_live(NULL));
    CHECK(!fake_slot_loaded(0u) && !fake_slot_loaded(1u));
    CHECK(send_sealed(CMD_REBOOT, NULL, 0u) == 1u && plain_is(fe, 3u) && g_hook_calls == 0u);

    CASE("a fresh session starts again at 1");
    CHECK(sec2c_auth(&C, &BLE) == 0);
    CHECK(send_sealed(CMD_REBOOT, NULL, 0u) == 1u && reply_is(ok, 3u) && C.rx_ctr == 1u);

    printf("sec2_ctr: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
