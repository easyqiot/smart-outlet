#! /usr/bin/env python3.6

import sys
import time
import struct
import base64
import socket
import argparse


__version__ = '0.1.0a'


if sys.argv[1] == '-V':
    print(__version__)
    sys.exit(0)


cli = argparse.ArgumentParser(
    prog=sys.argv[0],
    description='Utility for ESP8266 FOTA using easyq'
)
cli.add_argument('filename', help='Input file to download to device')
cli.add_argument('queue', help='Queue name')
cli.add_argument(
    'host',
    metavar='{HOST:}PORT',
    help='EasyQ server Address'
)
cli.add_argument('-V', '--version', action='store_true', help='Show version')
cli.add_argument(
    '-c', '--chunk-size',
    default=512,
    type=int,
    help='Show version'
)
args = cli.parse_args()

def push(s, d):
    s.sendall(b'PUSH %s INTO %s;\n' % (d, args.queue.encode()))


def main():
    host, port = args.host.split(':') if ':' in args.host else ('', args.host)
    host = socket.gethostbyname(host)
    port = int(port)
    total = 0
    ticks = 0
    with open(args.filename, 'rb') as f, \
            socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, port))
        s.sendall(b'LOGIN fota;\n')
        session = s.recv(20)
        print('Server: ', session)

        push(s, b'S')
        time.sleep(.1)
        while True:
            chunk = f.read(args.chunk_size)
            if not chunk:
                break

            size = len(chunk)
            total += size
            header = struct.pack('<H', size)
            chunk = base64.encodebytes(header + chunk)
            push(s, b'D' + chunk)
            if (ticks % 8 == 7):
                print(' %d:%x' % (ticks//8,  total))
                time.sleep(.7)
            else:
                print('.', end='', flush=True)
                time.sleep(.15)

            if size < args.chunk_size:
                break
            ticks += 1

        time.sleep(1)
        push(s, b'F')
        print('\nDone, total: %d' % total)
    return 0



if __name__ == '__main__':
    sys.exit(main())
