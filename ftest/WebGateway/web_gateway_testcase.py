import os
import unittest
import json
import time
from websocket import create_connection

class WebGatewayTestCase(unittest.TestCase):

    def setUp(self):
        time.sleep(1)

        self.WS_SERVER_URL = os.getenv('WS_SERVER_URL', "ws://127.0.0.1:8082")
        self.OPC_SERVER_URL = os.getenv('OPC_SERVER_URL', "opc.tcp://127.0.0.1:8889")
        self.SERVER_PKI_ROOT_DIR = os.path.join(os.getenv('SERVER_PKI_ROOT_DIR', '/tmp/'),
                                                'etc/OpcUaStack/ASNeG-Demo/pki')

        # TODO: Here the certs of client and server should be exchanged

        #
        # open web socket connection
        #
        self.ws = create_connection(self.WS_SERVER_URL)

        #
        # send login request to open opc ua session
        #
        req = {
            "Header": {
                "MessageType": "GW_LoginRequest",
                "ClientHandle": "client-handle"
            },
            "Body": {
                "DiscoveryUrl":  self.OPC_SERVER_URL
            }
        }
        print("SEND: ", json.dumps(req, indent=4))
        self.ws.send(json.dumps(req))

        str = self.ws.recv()
        print("RECV: ", str)
        res = json.loads(str)
        self.assertEqual(res['Header']['MessageType'], "GW_LoginResponse")
        self.assertEqual(res['Header']['ClientHandle'], "client-handle")
        self.assertEqual(res['Header']['StatusCode'], "0")
        self.assertIsNotNone(res['Body']['SessionId'])
        self.sessionId = res['Body']['SessionId']

        #
        # receive session status notify
        #
        str = self.ws.recv()
        print("RECV: ", str)
        res = json.loads(str)
        self.assertEqual(res['Header']['MessageType'], "GW_SessionStatusNotify")
        self.assertEqual(res['Header']['ClientHandle'], "client-handle")
        self.assertEqual(res['Header']['SessionId'], self.sessionId)
        self.assertEqual(res['Body']['SessionStatus'], "Connect")

    def tearDown(self):
        #
        # send logout request to close opc ua session
        #
        req = {
            "Header": {
                "MessageType": "GW_LogoutRequest",
                "ClientHandle": "client-handle",
                "SessionId": self.sessionId
            },
            "Body": {
            }

        }
        print("SEND: ", json.dumps(req, indent=4))
        self.ws.send(json.dumps(req))

        str = self.ws.recv()
        print("RECV: ", str)
        res = json.loads(str)
        self.assertEqual(res['Header']['MessageType'], "GW_LogoutResponse")
        self.assertEqual(res['Header']['ClientHandle'], "client-handle")
        self.assertEqual(res['Header']['SessionId'], self.sessionId)
        self.assertEqual(res['Header']['StatusCode'], "0")
        #
        # close web socket connection
        #
        self.ws.close()