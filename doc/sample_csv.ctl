#
# sample_csv.ctl -- Control file to load CSV input data
#
#    Copyright (c) 2007-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
#
TYPE = CSV                            # Input file type
TABLE = table_name                    # [<schema_name>.]table_name
INFILE = /path/to/input_data_file.csv # Input data location (absolute path)
QUOTE = "\""                          # Quoting character
ESCAPE = \                            # Escape character for Quoting
DELIMITER = ","                       # Delimiter
