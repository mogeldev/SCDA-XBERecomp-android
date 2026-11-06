"""Apply narrowly scoped Android import guards to the pinned upstream source."""
from pathlib import Path
import sys


def harden(text):
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned extract-xiso source changed: ' + old[:80])
        text = text.replace(old, new)

    replace('int main( int argc, char **argv ) {', 'int upstream_main( int argc, char **argv ) {')
    replace('int traverse_xiso( int in_xiso, dir_node *in_dir_node, xoff_t in_dir_start, char *in_path, modes in_mode, dir_node_avl **in_root, bool in_ll_compat ) {',
            'int traverse_xiso( int in_xiso, dir_node *in_dir_node, xoff_t in_dir_start, char *in_path, modes in_mode, dir_node_avl **in_root, bool in_ll_compat ) {\n\tif (++scda_depth > 64) scda_fail("ISO directory nesting exceeds 64");')
    replace('read_entry:\n', 'read_entry:\n\tscda_entry(in_xiso);\n')
    replace('dir->filename[ dir->filename_length ] = 0;',
            'dir->filename[ dir->filename_length ] = 0;\n\t\t\tscda_name(dir->filename, dir->filename_length);')
    replace('\n\treturn err;\n}\n\n\n#if 0\n#pragma mark -\n#endif\n\n\ndir_node_avl *avl_fetch',
            '\n\t--scda_depth;\n\treturn err;\n}\n\n\n#if 0\n#pragma mark -\n#endif\n\n\ndir_node_avl *avl_fetch')
    replace('if ( ( out = open( in_file->filename, WRITEFLAGS, 0644 ) ) == -1 ) open_err( in_file->filename );',
            'scda_extent(in_xiso, (xoff_t)in_file->start_sector * XISO_SECTOR_SIZE + s_xbox_disc_lseek, in_file->file_size);\n\t\t\tif ( ( out = open( in_file->filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600 ) ) == -1 ) open_err( in_file->filename );')
    replace('if (write(out, s_copy_buffer, read_size) != (int)read_size) {',
            'scda_space(read_size);\n\t\t\t\t\t\tif (write(out, s_copy_buffer, read_size) != (int)read_size) {')
    replace('exiso_log("\\nWARNING: File %s is truncated. Reported size: %u bytes, read size: %u bytes!", in_file->filename, in_file->file_size, i);',
            'scda_fail("Truncated ISO file");')
    return text


if __name__ == '__main__':
    Path(sys.argv[2]).write_text(harden(Path(sys.argv[1]).read_text()))
