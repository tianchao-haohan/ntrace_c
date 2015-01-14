#!/usr/bin/env python

#---------------------------------------------------------------------------------
# Name: pause.py
# Purpose:
#
# Time-stamp: <2015-01-15 00:19:35 Thursday by lzy>
#
# Author: zhengyu li
# Created: 24 May 2014
#
# Copyright (c) 2014 zhengyu li <lizhengyu419@gmail.com>
#---------------------------------------------------------------------------------

import json
import zmq

pauseBody = {}

pauseDict = {}
pauseDict ['command'] = 'pause'
pauseDict ['body'] = pauseBody
pauseJson = json.dumps (pauseDict)
print pauseJson

context = zmq.Context ()
request = context.socket (zmq.REQ)
request.connect ("tcp://127.0.0.1:58001")
request.send_json (pauseDict)
print request.recv_json ()