/* <filesystem>.
 *
 * Two halves, tested differently because they ARE different.
 *
 * `path` touches no filesystem. Every question it answers is about the
 * SHAPE of a string, so all of it runs here and all of it is checked
 * against the exact answers the standard specifies -- including the
 * three that surprise people:
 *
 *   `a / "/b"` is "/b", not "/a/b". An absolute right-hand side
 *     REPLACES. Quietly making it relative to something else would turn
 *     a bug into a silently wrong file.
 *   ".bashrc" is all STEM and has no extension. A leading dot is not an
 *     extension separator, and neither is the dot in "." or "..".
 *   `lexically_normal` resolves ".." by DELETING the previous
 *     component, which is wrong if that component is a symlink -- which
 *     is exactly why the name says "lexically" and why `canonical`
 *     exists and needs a filesystem.
 *
 * The OPERATIONS need lib/libc/os/backend.h's filesystem group, which
 * this target does not implement -- so what is checked is that each
 * fails HONESTLY: a system_error carrying errc::function_not_supported,
 * not a wrong answer. Two of them are checked for the opposite: status()
 * of a missing path and remove() of a missing path are NOT errors, they
 * are answers, and an implementation that conflates "not there" with
 * "cannot tell" makes a program decide a file is missing when the disk
 * is unreadable.
 */
#include "check.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

static bool enosys(const error_code &ec)
{ return ec == errc::function_not_supported; }

int main()
{
    /* ---- construction and comparison ---------------------------------- */
    {
        fs::path a("/usr/local/bin");
        fs::path b = a;
        CHECK(a == b);
        CHECK(a.string() == "/usr/local/bin");
        CHECK(string(a.c_str()) == "/usr/local/bin");
        CHECK(fs::path() == fs::path(""));
        CHECK(fs::path().empty());
        CHECK(!a.empty());
        CHECK(fs::path("a") < fs::path("b"));
        CHECK(fs::path("abc") != fs::path("abd"));
        /* Constructible from a string, a string_view and a pair of
         * iterators. */
        string s = "x/y";
        CHECK(fs::path(s) == fs::path("x/y"));
        CHECK(fs::path(string_view("x/y")) == fs::path("x/y"));
        CHECK(fs::path(s.begin(), s.end()) == fs::path("x/y"));
    }

    /* ---- decomposition -------------------------------------------------- */
    {
        fs::path p("/home/user/file.txt");
        CHECK(p.filename() == "file.txt");
        CHECK(p.stem() == "file");
        CHECK(p.extension() == ".txt");
        CHECK(p.parent_path() == "/home/user");
        CHECK(p.root_directory() == "/");
        CHECK(p.root_path() == "/");
        CHECK(p.root_name() == "");          /* no drive letters here */
        CHECK(p.relative_path() == "home/user/file.txt");
        CHECK(p.is_absolute() && !p.is_relative());
        CHECK(p.has_filename() && p.has_stem() && p.has_extension());
        CHECK(p.has_parent_path() && p.has_root_directory());
    }

    {
        fs::path p("file.txt");
        CHECK(p.filename() == "file.txt");
        CHECK(p.parent_path() == "");
        CHECK(!p.has_parent_path());
        CHECK(p.is_relative() && !p.is_absolute());
        CHECK(p.relative_path() == "file.txt");
    }

    /* The parent of a path directly under the root is the ROOT, not the
     * empty path -- which is what stops a walk upward from losing the
     * leading slash. */
    {
        CHECK(fs::path("/a").parent_path() == "/");
        CHECK(fs::path("/a").filename() == "a");
        CHECK(fs::path("/").filename() == "");
        CHECK(fs::path("/").parent_path() == "/");
        CHECK(fs::path("/").is_absolute());
    }

    /* A trailing separator means the filename is EMPTY, which is what
     * makes "dir/" name the directory rather than something in it. */
    {
        fs::path p("/a/b/");
        CHECK(p.filename() == "");
        CHECK(!p.has_filename());
        CHECK(p.parent_path() == "/a/b");
    }

    /* ---- the dot cases -------------------------------------------------- */
    {
        /* A LEADING dot is not an extension separator: a dotfile is all
         * stem. Getting this wrong turns ".bashrc" into an extension
         * with an empty name. */
        fs::path d(".bashrc");
        CHECK(d.stem() == ".bashrc");
        CHECK(d.extension() == "");
        CHECK(!d.has_extension());

        /* ... but a dotfile WITH an extension splits at the last dot. */
        fs::path d2(".config.json");
        CHECK(d2.stem() == ".config");
        CHECK(d2.extension() == ".json");

        /* "." and ".." are never stem-plus-extension. */
        CHECK(fs::path(".").stem() == ".");
        CHECK(fs::path(".").extension() == "");
        CHECK(fs::path("..").stem() == "..");
        CHECK(fs::path("..").extension() == "");

        /* The LAST dot wins. */
        fs::path m("archive.tar.gz");
        CHECK(m.stem() == "archive.tar");
        CHECK(m.extension() == ".gz");
        CHECK(m.stem().stem() == "archive");

        /* A trailing dot is an empty extension, which is still an
         * extension. */
        fs::path t("name.");
        CHECK(t.stem() == "name");
        CHECK(t.extension() == ".");
    }

    /* ---- appending, and the rule everyone meets once -------------------- */
    {
        fs::path a("/usr");
        CHECK(a / "bin" == "/usr/bin");
        CHECK(fs::path("/usr/") / "bin" == "/usr/bin");   /* no doubling */
        CHECK(fs::path("") / "bin" == "bin");
        /* An ABSOLUTE right-hand side replaces. */
        CHECK(fs::path("/usr") / "/etc" == "/etc");
        /* Appending nothing leaves a trailing separator, so the result
         * names the directory. */
        CHECK((fs::path("/usr") / "").string() == "/usr/");

        fs::path b("/usr");
        b /= "local";
        b /= "bin";
        CHECK(b == "/usr/local/bin");

        /* `+=` CONCATENATES -- no separator. Two operators because they
         * are two different things and one is nearly always wrong. */
        fs::path c("/tmp/file");
        c += ".txt";
        CHECK(c == "/tmp/file.txt");
        CHECK(c.extension() == ".txt");
    }

    /* ---- modifiers ------------------------------------------------------ */
    {
        fs::path p("/a/b/c.txt");
        p.replace_extension(".md");
        CHECK(p == "/a/b/c.md");
        p.replace_extension("log");          /* a dot is supplied */
        CHECK(p == "/a/b/c.log");
        p.replace_extension();               /* removed entirely */
        CHECK(p == "/a/b/c");
        CHECK(!p.has_extension());

        p.replace_filename("d.txt");
        CHECK(p == "/a/b/d.txt");
        p.remove_filename();
        CHECK(p == "/a/b/");
        p.clear();
        CHECK(p.empty());

        fs::path x("a"), y("b");
        x.swap(y);
        CHECK(x == "b" && y == "a");
    }

    /* ---- lexically_normal ----------------------------------------------- */
    {
        CHECK(fs::path("/a/b/../c").lexically_normal() == "/a/c");
        CHECK(fs::path("/a/./b").lexically_normal() == "/a/b");
        CHECK(fs::path("a/b/../../c").lexically_normal() == "c");
        CHECK(fs::path("/a/b/c/../../..").lexically_normal() == "/");
        /* An absolute path cannot rise above the root, so a ".." there
         * is dropped. */
        CHECK(fs::path("/../a").lexically_normal() == "/a");
        /* A RELATIVE one can: there is nothing above it to cancel, so
         * the ".." is kept. */
        CHECK(fs::path("../a").lexically_normal() == "../a");
        CHECK(fs::path("../../a").lexically_normal() == "../../a");
        CHECK(fs::path("a/../../b").lexically_normal() == "../b");
        /* An empty path normalises to ".", which is the path that names
         * where you are. */
        CHECK(fs::path("").lexically_normal() == ".");
        CHECK(fs::path(".").lexically_normal() == ".");
        CHECK(fs::path("/").lexically_normal() == "/");
        /* Repeated separators collapse. */
        CHECK(fs::path("/a//b///c").lexically_normal() == "/a/b/c");
    }

    /* ---- lexically_relative --------------------------------------------- */
    {
        CHECK(fs::path("/a/b/c").lexically_relative("/a/b") == "c");
        CHECK(fs::path("/a/b").lexically_relative("/a/b/c") == "..");
        CHECK(fs::path("/a/b/c").lexically_relative("/a/d") == "../b/c");
        CHECK(fs::path("/a/b").lexically_relative("/a/b") == ".");
        CHECK(fs::path("a/b").lexically_relative("a") == "b");
        /* No relative path crosses between absolute and relative: no
         * sequence of ".." gets there. */
        CHECK(fs::path("/a/b").lexically_relative("a").empty());
        CHECK(fs::path("a/b").lexically_relative("/a").empty());
        /* proximate falls back to the path itself when there is none. */
        CHECK(fs::path("/a/b").lexically_proximate("a") == "/a/b");
    }

    /* ---- iteration ------------------------------------------------------ */
    {
        /* The root directory is a component of its own, which is what
         * makes an absolute path distinguishable by iteration alone. */
        vector<string> parts;
        for (const fs::path &c : fs::path("/usr/local/bin"))
            parts.push_back(c.string());
        CHECK(parts.size() == 4);
        CHECK(parts[0] == "/" && parts[1] == "usr");
        CHECK(parts[2] == "local" && parts[3] == "bin");

        vector<string> rel;
        for (const fs::path &c : fs::path("a/b/c"))
            rel.push_back(c.string());
        CHECK(rel.size() == 3);
        CHECK(rel[0] == "a" && rel[2] == "c");

        int n = 0;
        for (const fs::path &c : fs::path("")) {
            (void)c;
            n++;
        }
        CHECK(n == 0);

        /* Repeated separators are one component, however many were
         * written. */
        vector<string> many;
        for (const fs::path &c : fs::path("//a//b"))
            many.push_back(c.string());
        CHECK(many.size() == 3);
        CHECK(many[0] == "/" && many[1] == "a" && many[2] == "b");
    }

    /* ---- file_status is a value, and its predicates are pure ------------ */
    {
        fs::file_status none;
        CHECK(none.type() == fs::file_type::none);
        CHECK(!fs::status_known(none));
        CHECK(!fs::exists(none));

        fs::file_status missing(fs::file_type::not_found);
        CHECK(fs::status_known(missing));
        CHECK(!fs::exists(missing));

        fs::file_status reg(fs::file_type::regular, fs::perms::owner_all);
        CHECK(fs::exists(reg) && fs::is_regular_file(reg));
        CHECK(!fs::is_directory(reg) && !fs::is_symlink(reg));
        CHECK(!fs::is_other(reg));
        CHECK(reg.permissions() == fs::perms::owner_all);

        fs::file_status dir(fs::file_type::directory);
        CHECK(fs::is_directory(dir) && !fs::is_regular_file(dir));

        fs::file_status fifo(fs::file_type::fifo);
        CHECK(fs::is_fifo(fifo) && fs::is_other(fifo));
    }

    /* perms is a bitmask, and the operators are what make it one. */
    {
        using fs::perms;
        perms p = perms::owner_read | perms::owner_write;
        CHECK((unsigned)p == 0600u);
        CHECK((unsigned)(p & perms::owner_read) != 0);
        CHECK((unsigned)(p & perms::group_read) == 0);
        CHECK((unsigned)perms::all == 0777u);
        CHECK((unsigned)(perms::all & ~perms::others_all) == 0770u);
        p |= perms::group_read;
        CHECK((unsigned)p == 0640u);
    }

    /* ---- the operations fail HONESTLY on a target with no filesystem ---- */
    {
        error_code ec;

        /* A missing path is an ANSWER, not an error -- but only when
         * the OS said "not there". With no filesystem at all it cannot
         * say that, so this is an error, and the two are distinguished.
         * That distinction is what stops a program deciding a file is
         * missing when the disk is unreadable. */
        fs::file_status st = fs::status("/nonexistent", ec);
        CHECK(ec || st.type() == fs::file_type::not_found);
        if (ec)
            CHECK(enosys(ec));

        ec.clear();
        (void)fs::file_size("/anything", ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        (void)fs::create_directory("/anything", ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        (void)fs::create_directories("/a/b/c", ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        (void)fs::current_path(ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        (void)fs::space("/", ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        (void)fs::last_write_time("/x", ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        fs::resize_file("/x", 0, ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        (void)fs::read_symlink("/x", ec);
        CHECK(ec && enosys(ec));

        ec.clear();
        fs::create_symlink("/a", "/b", ec);
        CHECK(ec && enosys(ec));
    }

    /* The throwing forms throw a filesystem_error, and it CARRIES THE
     * PATH -- which is the whole difference from a plain system_error.
     * "No such file or directory" is not a diagnosis. */
    {
        int caught = 0;
        try {
            (void)fs::file_size("/some/missing/file");
        } catch (const fs::filesystem_error &e) {
            caught = 1;
            CHECK(e.path1() == "/some/missing/file");
            CHECK(string(e.what()).find("/some/missing/file") != string::npos);
            CHECK(e.code() == errc::function_not_supported ||
                  e.code() == errc::no_such_file_or_directory);
        }
        CHECK(caught);

        /* It is a system_error, so a handler that knows only the
         * standard hierarchy still catches it. */
        int as_system = 0;
        try {
            (void)fs::file_size("/x");
        } catch (const system_error &) {
            as_system = 1;
        }
        CHECK(as_system);

        /* And the two-path form names both. */
        int two = 0;
        try {
            fs::rename("/a", "/b");
        } catch (const fs::filesystem_error &e) {
            two = 1;
            CHECK(e.path1() == "/a" && e.path2() == "/b");
        }
        CHECK(two);
    }

    /* remove() of something that is not there is FALSE, not an error:
     * the postcondition -- it is gone -- already holds. With no
     * filesystem it cannot be established, so this reports the failure
     * instead, and the test accepts either rather than pretending. */
    {
        error_code ec;
        bool r = fs::remove("/definitely/not/here", ec);
        CHECK(!r);
        CHECK(!ec || enosys(ec));
    }

    /* A directory_iterator over a directory it cannot open reports it
     * rather than silently yielding nothing -- an empty listing and an
     * unreadable directory are different answers. */
    {
        error_code ec;
        fs::directory_iterator it("/anything", ec);
        CHECK(ec && enosys(ec));
        CHECK(it == fs::directory_iterator());

        /* The default-constructed one is the end, and comparing two of
         * them is how a range ends. */
        CHECK(fs::directory_iterator() == fs::directory_iterator());
        CHECK(begin(fs::directory_iterator()) == fs::directory_iterator());
        CHECK(end(fs::directory_iterator()) == fs::directory_iterator());
    }

    {
        error_code ec;
        fs::recursive_directory_iterator it("/anything", ec);
        CHECK(ec && enosys(ec));
        CHECK(it == fs::recursive_directory_iterator());
    }

    /* directory_entry holds a path and answers about it; constructed
     * with a known type it does not need the filesystem at all, which
     * is what makes a listing cost one syscall per entry rather than
     * two. */
    {
        fs::directory_entry e(fs::path("/a/b.txt"), fs::file_type::regular);
        CHECK(e.path() == "/a/b.txt");
        CHECK(e.is_regular_file());
        CHECK(!e.is_directory());
        CHECK(e.exists());
        fs::directory_entry d(fs::path("/a/sub"), fs::file_type::directory);
        CHECK(d.is_directory() && !d.is_regular_file());
        CHECK(e != d);
        CHECK((e < d) != (d < e));
    }

    /* weakly_canonical does NOT require the path to exist, which is
     * what separates it from canonical -- so it works here where
     * canonical cannot. */
    {
        error_code ec;
        fs::path w = fs::weakly_canonical("/a/b/../c", ec);
        CHECK(!ec || enosys(ec));
        if (!ec)
            CHECK(w == "/a/c");
    }

    /* temp_directory_path has one answer on this target and gives it
     * rather than failing: a program that needs scratch space needs a
     * path. */
    {
        error_code ec;
        fs::path t = fs::temp_directory_path(ec);
        CHECK(!ec);
        CHECK(t == "/tmp");
    }

    DONE();
}
