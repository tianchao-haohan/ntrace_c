#!/usr/bin/env python

#---------------------------------------------------------------------------------
# Name: heartbeat.py
# Purpose:
#
# Time-stamp: <2014-12-23 13:40:25 Tuesday by lzy>
#
# Author: zhengyu li
# Created: 24 May 2014
#
# Copyright (c) 2014 zhengyu li <lizhengyu419@gmail.com>
#---------------------------------------------------------------------------------

import json
import zmq

heartbeatBody = {}
heartbeatBody ['agent_id'] = '12345'
heartbeatDict = {}
heartbeatDict ['command'] = 'heartbeat'
heartbeatDict ['body'] = heartbeatBody
heartbeatJson = json.dumps (heartbeatDict)
print heartbeatJson

context = zmq.Context ()
request = context.socket (zmq.REQ)
request.connect ("tcp://127.0.0.1:59000")
request.send_json (heartbeatDict)
print request.recv_json ()
