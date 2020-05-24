#include <errno.h>
#include <string.h>
#include <assert.h>
#include <async/jsonyield.h>
#include <async/jsondecoder.h>
#include <async/jsonencoder.h>
#include <async/queuestream.h>
#include <async/pacerstream.h>
#include <async/naiveencoder.h>
#include <async/tricklestream.h>
#include "asynctest.h"

typedef struct {
    async_t *async;
    yield_1 yield;
    jsondecoder_t *decoder;
    async_timer_t *timer;
    size_t pdu_count;
    size_t expected_pdu_count;
    VERDICT verdict;
} tester_t;

static void do_quit(tester_t *tester)
{
    action_1 quitter = { tester->async, (act_1) async_quit_loop };
    tester->timer = NULL;
    async_execute(tester->async, quitter);
    tester->async = NULL;
}

static void quit_test(tester_t *tester)
{
    async_timer_cancel(tester->async, tester->timer);
    do_quit(tester);
}

static void test_timeout(tester_t *tester)
{
    tlog("Test timeout");
    do_quit(tester);
}

static bool verify_data(json_thing_t *data)
{
    if (json_thing_type(data) != JSON_OBJECT)
        return false;
    json_thing_t *sweden = json_object_get(data, "Sweden");
    if (!sweden)
        return false;
    json_thing_t *population = json_object_get(sweden, "population");
    if (!population)
        return false;
    unsigned long long n;
    if (!json_cast_to_unsigned(population, &n) || n != 9900000)
        return false;
    return true;
}

static void verify_receive(tester_t *tester)
{
    if (!tester->async)
        return;
    json_thing_t *data;
    if (tester->decoder)
        data = jsondecoder_receive(tester->decoder);
    else
        data = yield_1_receive(tester->yield);
    if (data) {
        bool valid = verify_data(data);
        json_destroy_thing(data);
        if (!valid) {
            quit_test(tester);
            return;
        }
        action_1 verification_cb = { tester, (act_1) verify_receive };
        async_execute(tester->async, verification_cb);
        tester->pdu_count++;
        return;
    }
    switch (errno) {
        case EAGAIN:
            break;
        case 0:
            if (tester->pdu_count != tester->expected_pdu_count)
                tlog("Final pdu_count %u != %u (expected)",
                     (unsigned) tester->pdu_count,
                     (unsigned) tester->expected_pdu_count);
            else tester->verdict = PASS;
            if (tester->decoder)
                jsondecoder_close(tester->decoder);
            else
                yield_1_close(tester->yield);
            quit_test(tester);
            break;
        default:
            tlog("Errno %d from jsonyield_receive", errno);
            quit_test(tester);
    }
}

static json_thing_t *make_test_data(void)
{
    json_thing_t *data = json_make_object();
    json_thing_t *finland = json_make_object();
    json_add_to_object(finland, "capital", json_make_string("Helsinki"));
    json_add_to_object(finland, "population", json_make_unsigned(5500000));
    json_add_to_object(data, "Finland", finland);
    json_thing_t *sweden = json_make_object();
    json_add_to_object(sweden, "capital", json_make_string("Stockholm"));
    json_add_to_object(sweden, "population", json_make_unsigned(9900000));
    json_add_to_object(data, "Sweden", sweden);
    return data;
}

VERDICT test_json(void (*init_test_input)(tester_t *, json_thing_t *, action_1))
{
    async_t *async = make_async();
    tester_t tester = {
        .async = async,
        .verdict = FAIL,
    };
    json_thing_t *data = make_test_data();
    action_1 verification_cb = { &tester, (act_1) verify_receive };
    init_test_input(&tester, data, verification_cb);
    json_destroy_thing(data);
    action_1 timeout_cb = { &tester, (act_1) test_timeout };
    enum { MAX_DURATION = 10 };
    tlog("  max duration = %d s", MAX_DURATION);
    tester.timer =
        async_timer_start(async, async_now(async) + MAX_DURATION * ASYNC_S,
                          timeout_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_async(async);
    return posttest_check(tester.verdict);
}

static void init_jsonyield(tester_t *tester,
                           json_thing_t *data,
                           action_1 action)
{
    async_t *async = tester->async;
    queuestream_t *qstr = make_queuestream(async);
    unsigned i;
    for (i = 0; i < 200; i++) {
        bytestream_1 payload =
            jsonencoder_as_bytestream_1(json_encode(async, data));
        naiveencoder_t *naive_encoder =
            naive_encode(async, payload, '\0', '\33');
        queuestream_enqueue(qstr,
                            naiveencoder_as_bytestream_1(naive_encoder));
    }
    queuestream_terminate(qstr);
    pacerstream_t *pstr = pace_stream(async,
                                      queuestream_as_bytestream_1(qstr),
                                      5000, 10, 200);
    jsonyield_t *yield = open_jsonyield(async,
                                        pacerstream_as_bytestream_1(pstr),
                                        300);
    jsonyield_register_callback(yield, action);
    tester->yield = jsonyield_as_yield_1(yield);
    tester->expected_pdu_count = 200;
}

VERDICT test_jsonyield(void)
{
    return test_json(init_jsonyield);
}

static void init_jsondecoder(tester_t *tester,
                             json_thing_t *data,
                             action_1 action)
{
    async_t *async = tester->async;
    bytestream_1 payload =
        jsonencoder_as_bytestream_1(json_encode(async, data));
    tricklestream_t *trickle = open_tricklestream(async, payload, 0.01);
    jsondecoder_t *decoder =
        open_jsondecoder(async, tricklestream_as_bytestream_1(trickle), -1);
    jsondecoder_register_callback(decoder, action);
    tester->decoder = decoder;
    tester->expected_pdu_count = 1;
}

VERDICT test_jsondecoder(void)
{
    return test_json(init_jsondecoder);
}
