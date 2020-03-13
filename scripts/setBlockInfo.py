#!/usr/bin/env python3
import argparse

if __name__ == '__main__':
  parser = argparse.ArgumentParser ()
  parser.add_argument ('-b', '--blockname', type=str, required=True, help="Path to BBname.")
  parser.add_argument ('-o', '--blockname_id', type=str, required=True, help="Path to file blockname and id")

args = parser.parse_args ()
id_num = 0
if not args.blockname is None:
	if not args.blockname_id is None:
		with open(args.blockname_id, 'w') as fout:
			with open(args.blockname, 'r') as fin:
				for bbname in fin.readlines():
					bbinfo = bbname.strip("\n") + "," +str (id_num)
					print (bbinfo)
					fout.write(bbinfo + "\n")
					id_num = id_num + 1
			fout.close()
			


