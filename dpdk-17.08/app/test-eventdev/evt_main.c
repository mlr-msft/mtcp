/*
 *   BSD LICENSE
 *
 *   Copyright (C) Cavium, Inc 2017.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Cavium, Inc nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <rte_atomic.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_eventdev.h>

#include "evt_options.h"
#include "evt_test.h"

struct evt_options opt;
struct evt_test *test;

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\nSignal %d received, preparing to exit...\n",
				signum);
		/* request all lcores to exit from the main loop */
		*(int *)test->test_priv = true;
		rte_wmb();

		rte_eal_mp_wait_lcore();

		if (test->ops.eventdev_destroy)
			test->ops.eventdev_destroy(test, &opt);

		if (test->ops.ethdev_destroy)
			test->ops.ethdev_destroy(test, &opt);

		if (test->ops.mempool_destroy)
			test->ops.mempool_destroy(test, &opt);

		if (test->ops.test_destroy)
			test->ops.test_destroy(test, &opt);

		/* exit with the expected status */
		signal(signum, SIG_DFL);
		kill(getpid(), signum);
	}
}

static inline void
evt_options_dump_all(struct evt_test *test, struct evt_options *opts)
{
	evt_options_dump(opts);
	if (test->ops.opt_dump)
		test->ops.opt_dump(opts);
}

int
main(int argc, char **argv)
{
	uint8_t evdevs;
	int ret;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_panic("invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	evdevs = rte_event_dev_count();
	if (!evdevs)
		rte_panic("no eventdev devices found\n");

	/* Populate the default values of the options */
	evt_options_default(&opt);

	/* Parse the command line arguments */
	ret = evt_options_parse(&opt, argc, argv);
	if (ret) {
		evt_err("parsing on or more user options failed");
		goto error;
	}

	/* Get struct evt_test *test from name */
	test = evt_test_get(opt.test_name);
	if (test == NULL) {
		evt_err("failed to find requested test: %s", opt.test_name);
		goto error;
	}

	if (test->ops.test_result == NULL) {
		evt_err("%s: ops.test_result not found", opt.test_name);
		goto error;
	}

	/* Verify the command line options */
	if (opt.dev_id >= rte_event_dev_count()) {
		evt_err("invalid event device %d", opt.dev_id);
		goto error;
	}
	if (test->ops.opt_check) {
		if (test->ops.opt_check(&opt)) {
			evt_err("invalid command line argument");
			evt_options_dump_all(test, &opt);
			goto error;
		}
	}

	/* Check the eventdev capability before proceeding */
	if (test->ops.cap_check) {
		if (test->ops.cap_check(&opt) == false) {
			evt_info("unsupported test: %s", opt.test_name);
			evt_options_dump_all(test, &opt);
			ret = EVT_TEST_UNSUPPORTED;
			goto nocap;
		}
	}

	/* Dump the options */
	if (opt.verbose_level)
		evt_options_dump_all(test, &opt);

	/* Test specific setup */
	if (test->ops.test_setup) {
		if (test->ops.test_setup(test, &opt))  {
			evt_err("failed to setup test: %s", opt.test_name);
			goto error;

		}
	}

	/* Test specific mempool setup */
	if (test->ops.mempool_setup) {
		if (test->ops.mempool_setup(test, &opt)) {
			evt_err("%s: mempool setup failed", opt.test_name);
			goto test_destroy;
		}
	}

	/* Test specific ethdev setup */
	if (test->ops.ethdev_setup) {
		if (test->ops.ethdev_setup(test, &opt)) {
			evt_err("%s: ethdev setup failed", opt.test_name);
			goto mempool_destroy;
		}
	}

	/* Test specific eventdev setup */
	if (test->ops.eventdev_setup) {
		if (test->ops.eventdev_setup(test, &opt)) {
			evt_err("%s: eventdev setup failed", opt.test_name);
			goto ethdev_destroy;
		}
	}

	/* Launch lcores */
	if (test->ops.launch_lcores) {
		if (test->ops.launch_lcores(test, &opt)) {
			evt_err("%s: failed to launch lcores", opt.test_name);
			goto eventdev_destroy;
		}
	}

	rte_eal_mp_wait_lcore();

	/* Print the test result */
	ret = test->ops.test_result(test, &opt);
nocap:
	if (ret == EVT_TEST_SUCCESS) {
		printf("Result: "CLGRN"%s"CLNRM"\n", "Success");
	} else if (ret == EVT_TEST_FAILED) {
		printf("Result: "CLRED"%s"CLNRM"\n", "Failed");
		return EXIT_FAILURE;
	} else if (ret == EVT_TEST_UNSUPPORTED) {
		printf("Result: "CLYEL"%s"CLNRM"\n", "Unsupported");
	}

	return 0;
eventdev_destroy:
	if (test->ops.eventdev_destroy)
		test->ops.eventdev_destroy(test, &opt);

ethdev_destroy:
	if (test->ops.ethdev_destroy)
		test->ops.ethdev_destroy(test, &opt);

mempool_destroy:
	if (test->ops.mempool_destroy)
		test->ops.mempool_destroy(test, &opt);

test_destroy:
	if (test->ops.test_destroy)
		test->ops.test_destroy(test, &opt);
error:
	return EXIT_FAILURE;
}
