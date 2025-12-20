#! /usr/bin/python
# usage: python minsquare.py <data> <upperhull> > <output>

import sys
import os
import getopt

import corestats


def main():
    try:
        optlist, args = getopt.getopt(sys.argv[1:], 'd:h', ["deadline=", "help"])
    except getopt.GetoptError as err:
        print str(err)
        sys.exit(2)

    deadline = 0
    deadline_miss = 0;
    
    for opt, val in optlist:
        if opt in ("-h", "--help"):
            print args[0] + " [-d <deadline (ms)>]"
        elif opt in ("-d", "--deadline"):
            deadline = float(val)
        else:
            assert False, "unhandled option"


    print "deadline: ", deadline

    file1 = open(args[0], 'r')
            
    items = []
    while(True):
        line = file1.readline()
        if not line:
            break
        tokens = line.split();
    # print tokens
        try:
            num  = float(tokens[0])
        except ValueError:
            break
        items[len(items):] = [num]
        if deadline > 0 and num > deadline:
            deadline_miss += 1
            
    stats = corestats.Stats(items)
    print 
    print "----[", args[0], "]---"
    print "count: ", stats.count()
    print "deadline miss: ", deadline_miss
    print "deadline miss ratio: (%f)" % (float(deadline_miss) / stats.count())
    print "min: ", stats.min()
    print "avg: ", stats.avg()
    print "90pctile: ", stats.percentile(90)
    print "95pctile: ", stats.percentile(95)
    print "99pctile: ", stats.percentile(99)
    #print "median: ", stats.median()
    print "max: ", stats.max()
    print "stdev: ", stats.stdev()
    #avg  min max 99pctile
    print "LINE(avg|min|max|99pct|stdev): ", stats.avg(), \
        stats.min(), stats.max(), stats.percentile(99), stats.stdev()


if __name__ == "__main__":
    main()

