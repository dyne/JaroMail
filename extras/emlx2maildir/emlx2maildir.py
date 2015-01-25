#!/usr/bin/env python

# Copyright (C) 2009-2013 Mike Laiosa <mike@laiosa.org> and others.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

import optparse
import xml.sax, xml.sax.handler
import re, os, os.path, socket, time

class PlistHandler(xml.sax.handler.ContentHandler):
	def __init__(self):
		xml.sax.handler.ContentHandler.__init__(self)
		self.key = None
		def set_top(k,v):
			self.top = v
		self.stack = [set_top]
	def generate(self, value):
		self.stack[-1](self.key, value)
	def startElement(self, name, attrs):
		self.elem = name
		self.value = ""
		if name == "array":
			ar = []
			self.stack.append(ar)
			self.stack.append(lambda k,v: ar.append(v))
		elif name == "dict":
			d = {}
			def add(k, v):
				d[k] = v
			self.stack.append(d)
			self.stack.append(add)
	def endElement(self, name):
		if name == "string":
			self.generate(self.value)
		elif name == "integer":
			self.generate(long(self.value))
		elif name == "real":
			self.generate(float(self.value))
		elif name == "key":
			self.key = self.value
		elif name in ("dict", "array"):
			x = self.stack[-2]
			self.stack = self.stack[:-2]
			self.generate(x)
		elif name in ("plist", "data"):
			pass
		else:
			print "Unknown tag: %s" % name
	def characters(self, chars):
		self.value += chars

def parse_plist(plist_xml):
	# Remove any DOCTYPE declarations, to stop the XML parser from trying to
	# download the DTD.
	plist_xml = re.sub(r'<!DOCTYPE.*?"\s*>', "", plist_xml)
	p = PlistHandler()
	xml.sax.parseString(plist_xml, p)
	return p.top

FL_READ = (1<<0)
FL_DELETED = (1<<1)
FL_ANSWERED = (1<<2)
FL_ENCRYPTED = (1<<3)
FL_FLAGGED = (1<<4)
FL_RECENT = (1<<5)
FL_DRAFT = (1<<6)
FL_INITIAL = (1<<7)
FL_FORWARDED = (1<<8)
FL_REDIRECTED = (1<<9)
FL_SIGNED = (1<<23)
FL_IS_JUNK = (1<<24)
FL_IS_NOT_JUNK = (1<<25)
FL_JUNK_LEVEL_RECORDED = (1<<29)
FL_HIGHLIGHT_IN_TOC = (1<<30)

flag_mapping = [
	(FL_DRAFT, "D"),
	(FL_FLAGGED, "F"),
	((FL_FORWARDED | FL_REDIRECTED), "P"),
	(FL_ANSWERED, "R"),
	(FL_READ, "S"),
	(FL_DELETED, "T"),
]

hostname = socket.gethostname()
pid = os.getpid()
gSeq = 0

def md_filename(date, flags):
	global gSeq
	gSeq += 1
	return "%d.M%dP%dQ%d.%s:2,%s" % (date, time.time(), pid, gSeq, hostname, flags)

def convert_one(emlx_file, maildir):
	contents = open(emlx_file, "rb").read()
	boundry = contents.find("\x0a")
	length = long(contents[:boundry])
	body = contents[boundry+1:boundry+1+length]
	metadata = parse_plist(contents[boundry+1+length:])

	flags = ""
	if "flags" in metadata:
		for fl, let in flag_mapping:
			if metadata['flags'] & fl:
				flags += let

	date = long(metadata.get('date-sent', time.time()))
	filename = md_filename(date, flags)
	tmp_name = os.path.join(maildir, "tmp", filename)
	cur_name = os.path.join(maildir, "cur", filename)
	open(tmp_name, "wb").write(body)
	os.rename(tmp_name, cur_name)
	os.utime(cur_name, (date,date))

def emlx_message_dir(emlx_dir):
	msg_dir = os.path.join(emlx_dir + ".mbox", "Messages")
	if not os.path.isdir(msg_dir):
		msg_dir = os.path.join(emlx_dir + ".imapmbox", "Messages")
	if not os.path.isdir(msg_dir):
		return None
	return msg_dir

def emlx_message_dirs(emlx_dir):
	for x in os.listdir(emlx_dir):
		suffixes = [".sbd", ".mbox", ".imapmbox"]
		search = True
		for s in suffixes:
			if x.endswith(s):
				search = False
		if x == "Messages":
			msg_dir = os.path.join(emlx_dir,"Messages")
			yield msg_dir
		elif search:
			subfolder = os.path.join(emlx_dir, x)
			if	os.path.isdir(subfolder):
				print "Recursing into %r" % (subfolder)
				for tmp in emlx_message_dirs(subfolder):
					yield tmp

def emlx_messages(emlx_dir):
	if os.path.isdir(emlx_dir + ".mbox"):
		for msg_dir in emlx_message_dirs(emlx_dir + ".mbox"):
			for x in os.listdir(msg_dir):
				if x.endswith(".emlx") and x[0] != '.':
					yield os.path.join(msg_dir, x)

def emlx_subfolders(emlx_dir):
	if not os.path.isdir(emlx_dir):
		if os.path.isdir(emlx_dir + ".sbd"):
			emlx_dir = ".sbd"
		else:
			return
	for x in os.listdir(emlx_dir):
		suffixes = [".sbd", ".mbox", ".imapmbox"]
		for s in suffixes:
			if x.endswith(s):
				subfolder = os.path.join(emlx_dir, x[:-len(s)])
				for tmp in emlx_subfolders(os.path.join(emlx_dir, x)):
					yield tmp
				yield subfolder

def maildirmake(dir):
	for s in ["cur", "new", "tmp"]:
		if not os.path.exists(os.path.join(dir, s)):
			os.makedirs(os.path.join(dir, s))

def remove_slash(s):
	if len(s) and s[-1] == '/':
		return s[:-1]
	else:
		return s

def main():
	parser = optparse.OptionParser(usage="Usage: %prog [options] emlx_folder maildir")
	parser.add_option("-r", "--recursive", action="store_true", help="Recurse into subfolders")
	parser.add_option("-q", "--quiet", action="store_true", help="Only print error output")
	parser.add_option("--dry-run", action="store_true", help="Don't do anything")
	parser.add_option("--verbose", action="store_true", help="Displays lots of stuff")
	opts, args = parser.parse_args()

	def P(s):
		if not opts.quiet:
			print s
	def V(s):
		if opts.dry_run or opts.verbose:
			P(s)

	def dry(s, act, *args, **kwargs):
		V(s)
		if not opts.dry_run:
			return act(*args, **kwargs)

	if len(args) != 2:
		parser.error("Not enough arguments")

	tasks = [(remove_slash(args[0]), args[1] + '/')]
	while len(tasks):
		emlx_folder, maildir = tasks[-1]
		
		P("Converting %r -> %r" % (emlx_folder, maildir))
		tasks = tasks[:-1]
		dry("Making maildir %r" % maildir, maildirmake, maildir)
		for msg in emlx_messages(emlx_folder):
			dry("Converting message %r" % msg, convert_one, msg, maildir)
		if opts.recursive:
			for f in emlx_subfolders(emlx_folder):
				tasks.append((f, maildir + "." + os.path.basename(f)))

if __name__ == "__main__":
	main()
