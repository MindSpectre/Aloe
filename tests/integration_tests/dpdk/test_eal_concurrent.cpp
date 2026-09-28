#include <cstdlib>

#include <eal_arguments.hpp>
#include <gtest/gtest.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

    /// Starts the EAL, reports readiness, then holds it until told to stop.
    [[noreturn]] void run_child(int ready_write_end, int stop_read_end) {
        auto arguments = aloe::testing::unprivileged_eal_arguments();
        char status    = 'F';
        if (rte_eal_init(arguments.argc(), arguments.argv()) >= 0 && rte_eth_dev_count_avail() == 1) {
            status = 'R';
        }
        if (::write(ready_write_end, &status, 1) != 1) {
            ::_exit(EXIT_FAILURE);
        }
        char stop = 0;
        if (::read(stop_read_end, &stop, 1) != 1) {
            ::_exit(EXIT_FAILURE);
        }
        ::_exit(status == 'R' ? EXIT_SUCCESS : EXIT_FAILURE);
    }

}  // namespace

// A developer runs an example while the tests run, or CTest runs tests in
// parallel. Neither process may fail because the other one exists.
TEST(DpdkEal, TwoProcessesRunAtTheSameTime) {
    int ready_pipe[2] = {-1, -1};
    int stop_pipe[2]  = {-1, -1};
    ASSERT_EQ(::pipe(ready_pipe), 0);
    ASSERT_EQ(::pipe(stop_pipe), 0);

    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(stop_pipe[1]);
        run_child(ready_pipe[1], stop_pipe[0]);
    }
    ::close(ready_pipe[1]);
    ::close(stop_pipe[0]);

    // The child holds a running EAL from here until it is told to stop.
    char child_status = 0;
    ASSERT_EQ(::read(ready_pipe[0], &child_status, 1), 1);
    EXPECT_EQ(child_status, 'R') << "the first process could not start the EAL";

    auto arguments     = aloe::testing::unprivileged_eal_arguments();
    const int consumed = rte_eal_init(arguments.argc(), arguments.argv());
    EXPECT_GE(consumed, 0) << "the second process could not start the EAL: " << rte_strerror(rte_errno);
    if (consumed >= 0) {
        EXPECT_EQ(rte_eth_dev_count_avail(), 1);
        EXPECT_EQ(rte_eal_cleanup(), 0);
    }

    const char stop = 'S';
    ASSERT_EQ(::write(stop_pipe[1], &stop, 1), 1);
    int wait_status = 0;
    ASSERT_EQ(::waitpid(child, &wait_status, 0), child);
    EXPECT_TRUE(WIFEXITED(wait_status));
    EXPECT_EQ(WEXITSTATUS(wait_status), EXIT_SUCCESS);

    ::close(ready_pipe[0]);
    ::close(stop_pipe[1]);
}
