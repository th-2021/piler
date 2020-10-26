#!/usr/bin/python3
# -*- coding: utf-8 -*-

import MySQLdb as dbapi
import argparse
import configparser
import imaplib
import re
import sys

opts = {}
INBOX = 'INBOX'


def read_options(filename="", opts={}):
    s = "[piler]\n" + open(filename, 'r').read()
    config = configparser.ConfigParser()
    config.read_string(s)

    if config.has_option('piler', 'mysqlhost'):
        opts['dbhost'] = config.get('piler', 'mysqlhost')
    else:
        opts['dbhost'] = 'localhost'

    opts['username'] = config.get('piler', 'mysqluser')
    opts['password'] = config.get('piler', 'mysqlpwd')
    opts['database'] = config.get('piler', 'mysqldb')


def read_folder_list(conn):
    result = []

    rc, folders = conn.list()
    if opts['verbose']:
        print("Folders:", folders)

    for folder in folders:
        if opts['verbose']:
            print("Got folder", folder)

        if isinstance(folder, type(b'')):
            folder = folder.decode('utf-8')
        elif isinstance(folder, type(())):
            folder = re.sub(r'\{\d+\}$', '', folder[0]) + folder[1]

        # The regex should match ' "/" ' and ' "." '
        if folder:
            f = re.split(r' \"[\/\.]\" ', folder)
            result.append(f[1])

    return [x for x in result if x not in opts['skip_folders']]


def process_folder(conn, folder):
    if opts['verbose']:
        print("Processing {}".format(folder))

    rc, data = conn.select(folder)
    n = int(data[0])
    if opts['verbose']:
        print("Folder {} has {} messages".format(folder, n))

    if n > 0:
        rc, data = conn.search(None, 'ALL')
        for num in data[0].split():
            rc, data = conn.fetch(num, '(RFC822)')
            if opts['verbose']:
                print(rc, num)
            opts['counter'] += 1
            with open("{}.eml".format(opts['counter']), "wb") as f:
                f.write(data[0][1])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--config", type=str, help="piler.conf path",
                        default="/etc/piler/piler.conf")
    parser.add_argument("-s", "--server", type=str, help="imap server")
    parser.add_argument("-P", "--port", type=int, help="port number", default=143)
    parser.add_argument("-u", "--user", type=str, help="imap user")
    parser.add_argument("-p", "--password", type=str, help="imap password")
    parser.add_argument("-x", "--skip-list", type=str, help="IMAP folders to skip",
                        default="junk,trash,spam,draft")
    parser.add_argument("-f", "--folders", type=str,
                        help="Comma separated list of IMAP folders to download")
    parser.add_argument("-i", "--import-from-table", help="Read imap conn data from import table", action='store_true')
    parser.add_argument("-v", "--verbose", help="verbose mode", action='store_true')

    args = parser.parse_args()

    print(args)
    if not bool(args.import_from_table or args.server):
        print("Please specify either --import-from-table or --server <imap host>")
        sys.exit(1)

    opts['skip_folders'] = args.skip_list.split(',')
    opts['verbose'] = args.verbose
    opts['counter'] = 0
    opts['db'] = None

    server = ''
    user = ''
    password = ''

    if args.import_table:
        read_options(args.config, opts)
        try:
            opts['db'] = dbapi.connect(opts['dbhost'], opts['username'],
                                       opts['password'], opts['database'])

            cursor = opts['db'].cursor()
            cursor.execute("SELECT server, username, password FROM import WHERE started=0")

            row = cursor.fetchone()
            if row:
                (server, user, password) = row
            else:
                print("Nothing to read from import table")
                sys.exit(0)

        except dbapi.DatabaseError as e:
            print("Error %s" % e)
    else:
        server = args.server
        user = args.user
        password = args.password

    if opts['verbose']:
        print("Skipped folder list: {}".format(opts['skip_folders']))

    if args.port == 993:
        conn = imaplib.IMAP4_SSL(server)
    else:
        conn = imaplib.IMAP4(server)

    conn.login(user, password)
    conn.select()

    if args.folders:
        folders = args.folders.split(',')
    else:
        folders = read_folder_list(conn)

    if opts['verbose']:
        print("Folders will be processed: {}".format(folders))

    for folder in folders:
        process_folder(conn, folder)

    conn.close()

    if opts['db']:
        opts['db'].close()

    print("Processed {} messages".format(opts['counter']))

if __name__ == "__main__":
    main()
