#!/usr/bin/python
'''
Python script that takes a the name of a ceph admin socket and writes
a PCP JSON PMDA metadata file that describes the ceph perf counters.
'''
import json
import sys, getopt
import traceback
from collections import OrderedDict
import subprocess

COMMA_NEEDED = 0
VERBOSE = 0

# From ceph's src/common/perf_counters.h:
PERFCOUNTER_NONE = 0
PERFCOUNTER_TIME = 0x1
PERFCOUNTER_U64 = 0x2
PERFCOUNTER_LONGRUNAVG = 0x4
PERFCOUNTER_COUNTER = 0x8

def decode_type(indent, type_val):
    ''' Decode ceph types. '''
    names = []
    output = []

    # Strangely enough, a ceph type value can encode more than 1
    # type. For instance:
    #
    # - type 5: LONGRUNAVG + TIME
    #   "avgcount" integer
    #   "sum": float
    # - type 6: LONGRUNAVG + U64
    #   "avgcount" integer
    #   "sum": integer
    # - type 10: U64 + COUNTER
    #   This one is really a single value, just an integer

    if type_val & PERFCOUNTER_TIME:
        # When this one is specififed, the current item has a subitem
        # named 'sum' which is a float.
        #
        # FIXME: We don't handle floats yet. For now, just raise an
        # error.
        type_val &= ~PERFCOUNTER_TIME
        #names.append('sum')
        #output.append("%s\"type\": \"FLOAT?\"\n" % indent)
        raise TypeError
    if type_val & PERFCOUNTER_U64:
        type_val &= ~PERFCOUNTER_U64
        names.append('sum')

        if type_val & PERFCOUNTER_COUNTER:
            # This isn't a second value.
            type_val &= ~PERFCOUNTER_COUNTER
            output.append("%s\"type\": \"integer\",\n"
                          "%s\"units\": \"count\"\n" % (indent, indent))
        else:
            output.append("%s\"type\": \"integer\"\n" % indent)
    if type_val & PERFCOUNTER_LONGRUNAVG:
        type_val &= ~PERFCOUNTER_LONGRUNAVG
        names.append('avgcount')
        output.append("%s\"type\": \"integer\"\n" % indent)
    if type_val & PERFCOUNTER_COUNTER:
        type_val &= ~PERFCOUNTER_COUNTER
        names.append('count')
        output.append("%s\"type\": \"integer\",\n"
                      "%s\"units\": \"count\"\n" % (indent, indent))
    if type_val != 0:
        sys.stderr.write("Unknown type value: 0x%x\n" % type_val)
        raise TypeError
    return (names, output)

def generate_pcp_name(name):
    '''
    PCP has strict rules for names. Metric names must start with an
    alphabetic character. The rest of the characters must be
    alphanumeric or an '_'.

    Ceph names seem to match the above pretty well, but can have '-'
    and/or ':' in them. Let's translate all those to '_'.
    '''
    name = name.replace('-', '_')
    name = name.replace(':', '_')
    return name

def output_metric(out, indent, full_name, pointer, type_str):
    ''' Output a metric. '''
    global COMMA_NEEDED

    if COMMA_NEEDED:
        out.write(",\n")
    COMMA_NEEDED = 1

    out.write("%s{\n" % indent)
    out.write("  %s\"name\": \"%s\",\n" % (indent, full_name))
    out.write("  %s\"pointer\": \"%s\",\n" % (indent, pointer))
    out.write(type_str)
    out.write("%s}" % (indent))

def handle_json_dict(out, indent, root_name, json_dict):
    ''' Process a JSON schema dictionary. '''
    pcp_root_name = generate_pcp_name(root_name)
    if VERBOSE and pcp_root_name != root_name:
        sys.stderr.write("Changed '%s' to '%s'\n" % (root_name, pcp_root_name))
    for (key, value) in json_dict.iteritems():
        if not isinstance(value, OrderedDict):
            sys.stderr.write("Value isn't an OrderedDict?\n")
            sys.exit(1)

        # Decode 'type' field
        error = 0
        names = []
        output = []
        for (subkey, subvalue) in value.iteritems():
            if subkey != 'type':
                sys.stderr.write("Item '%s' value isn't 'type'\n" % subkey)
                sys.exit(1)
            try:
                (names, output) = decode_type(indent + "  ", subvalue)
            except TypeError:
                sys.stderr.write("Skipping %s.%s\n" % (root_name, key))
                error = 1
                break
        if error:
            continue

        # Output metadata
        pcp_name = generate_pcp_name(key)
        if len(names) == 0:
            sys.stderr.write("No 'type' field found for '%s'\n" % key)
            sys.exit(1)
        elif len(names) == 1:
            output_metric(out, indent, ("%s.%s" % (pcp_root_name, pcp_name)),
                          ("/%s/%s" % (root_name, key)), output[0])
        else:
            for i in xrange(0, len(names)):
                output_metric(out, indent,
                              ("%s.%s.%s" % (pcp_root_name, pcp_name,
                                             names[i])),
                              ("/%s/%s/%s" % (root_name, key, names[i])),
                              output[i])

def usage():
    ''' Print a usage message. '''
    sys.stderr.write("Usage: %s [-v] [-o FILE] ADMIN_SOCKET\n" % sys.argv[0])
    sys.stderr.write("Example: %s /var/run/ceph/ceph-osd.0.asok\n"
                     % sys.argv[0])

def main():
    ''' Main function. '''
    global VERBOSE

    if len(sys.argv) < 2:
        usage()
        sys.exit(1)

    # By default, output goes to a file named 'metadata.json'.
    outfile = "metadata.json"

    # Handle options.
    try:
        (opts, args) = getopt.getopt(sys.argv[1:], 'o:v')
    except getopt.GetoptError as err:
        sys.stderr.write("%s\n" % err)
        usage()
        sys.exit(1)
    for (opt, arg) in opts:
        if opt == '-o':
            outfile = arg
        elif opt == '-v':
            VERBOSE = 1
        else:
            sys.stderr.write("Unknown option '%s'\n" % opt)

    # Try to open the output file.
    fobj = open(outfile, 'w')

    # Make sure we have a ceph admin socket.
    if len(args) != 1:
        sys.stderr.write("Error, too many ceph admin sockets specified: %s\n"
                         % ' '.join(args))
        usage()
        sys.exit(1)

    # Run the ceph command to get the JSON schema.
    try:
        cmd_args = ['ceph', '--admin-daemon', args[0], 'perf', 'schema']
        pobj = subprocess.Popen(cmd_args, close_fds=True,
                                stdout=subprocess.PIPE)
        (out, dummy) = pobj.communicate()
    except (OSError, ValueError):
        sys.stderr.write("Couldn't run ceph command: %s\n"
                         % ' '.join(cmd_args))
        sys.stderr.write("%s\n" % traceback.format_exc())
        sys.exit(1)

    # Try to convert the schema to JSON objects.
    try:
        json_data = json.loads(out, object_pairs_hook=OrderedDict)
    except ValueError:
        sys.stderr.write("Couldn't parse JSON schema.\n")
        sys.stderr.write("%s" % traceback.format_exc())
        sys.exit(1)

    # Output the metadata file based on the ceph JSON schema.
    fobj.write("{\n")
    indent = "  "
    fobj.write("%s\"data-exec\": \"/usr/bin/ceph --admin-daemon"
               " %s perf dump\",\n" % (indent, args[0]))
    fobj.write("%s\"metrics\": [\n" % indent)
    for (key, value) in json_data.iteritems():
        if isinstance(value, OrderedDict):
            handle_json_dict(fobj, indent + "  ", key, value)
    fobj.write("\n%s]\n" % indent)
    fobj.write("}\n")

if __name__ == "__main__":
    main()
