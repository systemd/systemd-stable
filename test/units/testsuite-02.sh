#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

NPROC=$(nproc)
MAX_QUEUE_SIZE=${NPROC:-2}
TESTS_GLOB=${TESTS_GLOB:-test-*}
mapfile -t TEST_LIST < <(find /usr/lib/systemd/tests/ -maxdepth 1 -type f -name "${TESTS_GLOB}")

# Reset state
rm -fv /failed-tests /skipped-tests /skipped

if ! systemd-detect-virt -qc; then
    # Make sure ping works for unprivileged users (for test-bpf-firewall)
    sysctl net.ipv4.ping_group_range="0 2147483647"
fi

# Check & report test results
# Arguments:
#   $1: test path
#   $2: test exit code
function report_result() {
    if [[ $# -ne 2 ]]; then
        echo >&2 "check_result: missing arguments"
        exit 1
    fi

    local name="${1##*/}"
    local ret=$2

    if [[ $ret -ne 0 && $ret != 77 ]]; then
        echo "$name failed with $ret"
        echo "$name" >>/failed-tests
        {
            echo "--- $name begin ---"
            cat "/$name.log"
            echo "--- $name end ---"
        } >>/failed
    elif [[ $ret == 77 ]]; then
        echo "$name skipped"
        echo "$name" >>/skipped-tests
        {
            echo "--- $name begin ---"
            cat "/$name.log"
            echo "--- $name end ---"
        } >>/skipped
    else
        echo "$name OK"
        echo "$name" >>/testok
    fi

    systemd-cat echo "--- $name ---"
    systemd-cat cat "/$name.log"
}

set +x
# Associative array for running tasks, where running[test-path]=PID
declare -A running=()
for task in "${TEST_LIST[@]}"; do
    # If there's MAX_QUEUE_SIZE running tasks, keep checking the running queue
    # until one of the tasks finishes, so we can replace it.
    while [[ ${#running[@]} -ge $MAX_QUEUE_SIZE ]]; do
        for key in "${!running[@]}"; do
            if ! kill -0 "${running[$key]}" &>/dev/null; then
                # Task has finished, report its result and drop it from the queue
                wait "${running[$key]}" && ec=0 || ec=$?
                report_result "$key" $ec
                unset "running[$key]"
                # Break from inner for loop and outer while loop to skip
                # the sleep below when we find a free slot in the queue
                break 2
            fi
        done

        # Precisely* calculated constant to keep the spinlock from burning the CPU(s)
        sleep 0.01
    done

    if [[ -x $task ]]; then
        echo "Executing test '$task'"
        log_file="/${task##*/}.log"
        $task &>"$log_file" &
        running[$task]=$!
    fi
done

# Wait for remaining running tasks
for key in "${!running[@]}"; do
    echo "Waiting for test '$key' to finish"
    wait ${running[$key]} && ec=0 || ec=$?
    report_result "$key" $ec
    unset "running[$key]"
done

set -x

# Test logs are sometimes lost, as the system shuts down immediately after
journalctl --sync

exit 0
