'''
copyright: Copyright (C) 2015-2022, Wazuh Inc.

           Created by Wazuh, Inc. <info@wazuh.com>.

           This program is free software; you can redistribute it and/or modify it under the terms of GPLv2

type: integration

brief: This module verifies the correct behavior of authd under different messages in a Cluster scenario (for Worker)

components:
    - authd

targets:
    - manager

daemons:
    - wazuh-authd
    - wazuh-clusterd

os_platform:
    - linux

os_version:
    - Arch Linux
    - Amazon Linux 2
    - Amazon Linux 1
    - CentOS 8
    - CentOS 7
    - Debian Buster
    - Red Hat 8
    - Ubuntu Focal
    - Ubuntu Bionic

tags:
    - enrollment
'''
import os
import subprocess
import time
from pathlib import Path

import pytest
from wazuh_testing.modules.authd.utils import CLUSTER_DATA_HEADER_SIZE, cluster_msg_build
from wazuh_testing.constants.paths.logs import WAZUH_PATH, WAZUH_CLUSTER_LOGS_PATH
from wazuh_testing.utils.wazuh import DEFAULT_SSL_REMOTE_ENROLLMENT_PORT
from wazuh_testing.tools.mitm import ManInTheMiddle
from wazuh_testing.utils.configuration import load_configuration_template, get_test_cases_data

from . import CONFIGURATIONS_FOLDER_PATH, TEST_CASES_FOLDER_PATH

# Marks
pytestmark = [pytest.mark.linux, pytest.mark.tier(level=0), pytest.mark.server]


# Configurations
class WorkerMID(ManInTheMiddle):

    def __init__(self, address, family='AF_UNIX', connection_protocol='TCP', func: callable = None):
        self.cluster_input = None
        self.cluster_output = None
        super().__init__(address, family, connection_protocol, self.verify_message)

    def set_cluster_messages(self, cluster_input, cluster_output):
        self.cluster_input = cluster_input
        self.cluster_output = cluster_output

    def verify_message(self, data: bytes):
        if len(data) > CLUSTER_DATA_HEADER_SIZE:
            message = data[CLUSTER_DATA_HEADER_SIZE:]
            response = cluster_msg_build(command=b'send_sync', counter=2, payload=bytes(self.cluster_output.encode()),
                                         encrypt=False)[0]
            print(f'Received message from wazuh-authd: {message}')
            print(f'Response to send: {self.cluster_output}')
            self.pause()
            return response
        else:
            raise ConnectionResetError('Invalid cluster message!')

    def pause(self):
        self.event.set()

    def restart(self):
        self.event.clear()


# Configurations
test_configuration_path = Path(CONFIGURATIONS_FOLDER_PATH, 'configuration_wazuh_authd.yaml')
test_cases_path = Path(TEST_CASES_FOLDER_PATH, 'cases_authd_worker.yaml')
test_configuration, test_metadata, test_cases_ids = get_test_cases_data(test_cases_path)
test_configuration = load_configuration_template(test_configuration_path, test_configuration, test_metadata)


# Variables
log_monitor_paths = [WAZUH_CLUSTER_LOGS_PATH]
cluster_socket_path = os.path.join(os.path.join(WAZUH_PATH, 'queue', 'cluster', 'c-internal.sock'))
ossec_authd_socket_path = ("localhost", DEFAULT_SSL_REMOTE_ENROLLMENT_PORT)
receiver_sockets_params = [(ossec_authd_socket_path, 'AF_INET', 'SSL_TLSv1_2')]

mitm_master = WorkerMID(address=cluster_socket_path, family='AF_UNIX', connection_protocol='TCP')

monitored_sockets_params = [('wazuh-clusterd', mitm_master, True), ('wazuh-authd', None, True)]
receiver_sockets, monitored_sockets = None, None  # Set in the fixtures


# Fixtures
@pytest.fixture(scope='function')
def set_up_groups(request, get_current_test_case):
    """
    Set the pre-defined groups.
    """
    groups = get_current_test_case.get('groups', [])

    for group in groups:
        subprocess.call(['/var/ossec/bin/agent_groups', '-a', '-g', f'{group}', '-q'])

    yield

    for group in groups:
        subprocess.call(['/var/ossec/bin/agent_groups', '-r', '-g', f'{group}', '-q'])


# Tests
@pytest.mark.parametrize('test_configuration,test_metadata', zip(test_configuration, test_metadata), ids=test_cases_ids)
def test_ossec_auth_messages(test_configuration, test_metadata, set_up_groups, configure_sockets_environment,
                             connect_to_sockets_module, wait_for_authd_startup_module):
    '''
    description:
        Checks that every message from the agent is correctly formatted for master,
        and every master response is correctly parsed for agent.

    wazuh_min_version:
        4.2.0

    tier: 0

    parameters:
        - get_configuration:
            type: fixture
            brief: Get the configuration of the test.
        - set_up_groups:
            type: fixture
            brief: Set the pre-defined groups.
        - configure_environment:
            type: fixture
            brief: Configure a custom environment for testing.
        - configure_sockets_environment:
            type: fixture
            brief: Configure the socket listener to receive and send messages on the sockets.
        - connect_to_sockets_module:
            type: fixture
            brief: Bind to the configured sockets at module scope.
        - wait_for_authd_startup_module:
            type: fixture
            brief: Waits until Authd is accepting connections.
        - get_current_test_case:
            type: fixture
            brief: gets the current test case from the tests' list

    assertions:
        - The 'port_input' from agent is formatted to 'cluster_input' for master
        - The 'cluster_output' response from master is correctly parsed to 'port_output' for agent

    input_description:
        Different test cases are contained in an external YAML file (worker_messages.yaml) which includes
        the different possible registration requests and the expected responses.

    expected_output:
        - Registration request responses on Authd socket
    '''
    test_case = test_metadata

    for stage in test_case:
        # Push expected info to mitm queue
        mitm_master.set_cluster_messages(stage['cluster_input'], stage['cluster_output'])
        # Reopen socket (socket is closed by manager after sending message with client key)
        mitm_master.restart()
        receiver_sockets[0].open()
        expected = stage['port_output']
        message = stage['port_input']
        receiver_sockets[0].send(message, size=False)
        timeout = time.time() + 10
        response = ''
        while response == '':
            response = receiver_sockets[0].receive().decode()
            if time.time() > timeout:
                raise ConnectionResetError('Manager did not respond to sent message!')
        clusterd_queue = monitored_sockets[0]
        # callback lambda function takes out tcp header and decodes binary to string
        results = clusterd_queue.get_results(callback=(lambda y: [x[CLUSTER_DATA_HEADER_SIZE:].decode() for x in y]),
                                             timeout=1, accum_results=1)
        assert response[:len(expected)] == expected, \
            'Failed test case {}: Response was: {} instead of: {}'.format(set_up_groups['name'], response, expected)
        # Assert monitored sockets
        assert results[0] == stage['cluster_input'], 'Expected clusterd input message does not match'
        assert results[1] == stage['cluster_output'], 'Expected clusterd output message does not match'
