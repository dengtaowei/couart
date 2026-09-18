#include "couart.h"
#include "test_main.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_valid_name(void)
{
    ASSERT(couart_valid_name("demo"), "alnum");
    ASSERT(couart_valid_name("USB1"), "mixed");
    ASSERT(couart_valid_name("a_b-c"), "dash underscore");
    ASSERT(!couart_valid_name(""), "empty");
    ASSERT(!couart_valid_name("a/b"), "slash");
    ASSERT(!couart_valid_name("has space"), "space");
    ASSERT(!couart_valid_name(NULL), "null");
}

static void test_paths(void)
{
    char dir[256], sock[256], seat[256];
    ASSERT_INT_EQ(couart_instance_dir(dir, sizeof(dir), "demo"), 0);
    ASSERT(strstr(dir, "/couart/demo") != NULL, "instance dir suffix");
    ASSERT_INT_EQ(couart_control_path(sock, sizeof(sock), "demo"), 0);
    ASSERT(strstr(sock, "/couart/demo/control.sock") != NULL, "control sock");
    ASSERT_INT_EQ(couart_seat_path(seat, sizeof(seat), "demo", "console"), 0);
    ASSERT(strstr(seat, "/couart/demo/console") != NULL, "console seat");
    ASSERT_INT_EQ(couart_park_path(sock, sizeof(sock), "demo"), 0);
    ASSERT(strstr(sock, "/couart/demo/park.sock") != NULL, "park sock");
}

int main(void)
{
    printf("test_path\n");
    RUN_TEST(test_valid_name);
    RUN_TEST(test_paths);
    TEST_REPORT();
}
