#include <git2.h>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <functional>
#include <iostream>
#include <map>
#include <iomanip>
#include <random>
#include <windows.h>

// USER_NAME, USER_GHP_TOKEN, USER_URL_REPO
#include "FileName.h"

inline std::string Utf8ToWindows1251(const std::string& utf8_str) {
    if (utf8_str.empty()) return "";

    int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    if (wide_size == 0) return utf8_str;

    std::wstring wide_str(wide_size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, &wide_str[0], wide_size);

    int ansi_size = WideCharToMultiByte(CP_ACP, 0, wide_str.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ansi_size == 0) return utf8_str;

    std::string result(ansi_size, '\0');
    WideCharToMultiByte(CP_ACP, 0, wide_str.c_str(), -1, &result[0], ansi_size, nullptr, nullptr);

    result.pop_back();
    return result;
}

inline std::string make_random_name() {
    static std::random_device rd;
    uint64_t value = (static_cast<uint64_t>(rd()) << 32) | rd();
    char buf[13];
    snprintf(buf, sizeof(buf), "%012llx", value);
    return std::string(buf);
}

namespace GitWrapper {

    class GitException : public std::runtime_error {
    public:
        explicit GitException(const std::string& message)
            : std::runtime_error(message) {}

        explicit GitException(int error_code)
            : std::runtime_error(GetLastError(error_code)) {}

        static std::string GetLastError(int error_code) {
            const git_error* err = git_error_last();
            if (err && err->message) {
                return std::string("Git error ") + std::to_string(error_code) +
                    ": " + err->message;
            }
            return "Git error " + std::to_string(error_code);
        }

        static void Check(int error_code) {
            if (error_code < 0) {
                throw GitException(error_code);
            }
        }
    };

    struct GitVersion {
        int major = -1;
        int minor = -1;
        int patch = -1;

        std::string ToString() const {
            return std::to_string(major) + "." +
                std::to_string(minor) + "." +
                std::to_string(patch);
        }
    };

    struct GitSignature {
        std::string name;
        std::string email;
        time_t time = 0;
        int offset = 0;

        std::string ToString() const {
            return name + " <" + email + ">";
        }
    };

    struct GitCommitInfo {
        std::string id;
        std::string message;
        GitSignature author;
        GitSignature committer;
        time_t commit_time = 0;
        std::vector<std::string> parents;
    };

    struct GitCredentials {

        enum class Type
        {
            NONE,
            TOKEN,
            SSH_KEY,
        };

        Type type = Type::NONE;

        std::string username;
        std::string token;

        std::string ssh_private_key_path;
        std::string ssh_public_key_path;
        std::string ssh_passphrase;

        int attempt_count = 0;

        static GitCredentials Anonymous() {
            GitCredentials creds;
            creds.type = Type::NONE;
            return creds;
        }

        static GitCredentials SSHKey(
            const std::string& private_key_path,
            const std::string& public_key_path = "",
            const std::string& passphrase = "") {
            GitCredentials creds;
            creds.type = Type::SSH_KEY;
            creds.ssh_private_key_path = private_key_path;
            creds.ssh_public_key_path = public_key_path;
            creds.ssh_passphrase = passphrase;
            return creds;
        }

        static GitCredentials Token(const std::string& username, const std::string& token) {
            GitCredentials creds;
            creds.type = Type::TOKEN;
            creds.username = username;
            creds.token = token;
            return creds;
        }
    };

    class GitStrArray {
    private:
        git_strarray array{};
        bool owned = false;

    public:
        GitStrArray() = default;

        ~GitStrArray() { Clear(); }

        GitStrArray(const GitStrArray&) = delete;
        GitStrArray& operator=(const GitStrArray&) = delete;

        GitStrArray(GitStrArray&& o) noexcept
            : array(o.array), owned(o.owned) {
            o.array = {};
            o.owned = false;
        }

        GitStrArray& operator=(GitStrArray&& o) noexcept {
            if (this != &o) {
                Clear();
                array = o.array;
                owned = o.owned;
                o.array = {};
                o.owned = false;
            }
            return *this;
        }

        void Load(int (*func)(git_strarray*, git_repository*), git_repository* repo) {
            Clear();
            GitException::Check(func(&array, repo));
            owned = true;
        }

        void Clear() {
            if (owned && array.strings) {
                git_strarray_dispose(&array);
            }
            array = {};
            owned = false;
        }

        bool IsValid() const { return array.strings != nullptr; }
        size_t Count() const { return array.count; }

        const char* operator[](size_t i) const {
            return (i < array.count) ? array.strings[i] : nullptr;
        }

        std::vector<std::string> ToVector() const {
            std::vector<std::string> result;
            result.reserve(array.count);
            for (size_t i = 0; i < array.count; ++i) {
                result.emplace_back(array.strings[i] ? array.strings[i] : "");
            }
            return result;
        }

        git_strarray* GetRaw() { return &array; }
        const git_strarray* GetRaw() const { return &array; }
    };

    class GitRemote {
    private:
        git_remote* remote = nullptr;
        bool owned = false;

    public:
        GitRemote() = default;
        ~GitRemote() { Clear(); }

        GitRemote(const GitRemote&) = delete;
        GitRemote& operator=(const GitRemote&) = delete;

        GitRemote(GitRemote&& o) noexcept
            : remote(o.remote), owned(o.owned) {
            o.remote = nullptr;
            o.owned = false;
        }

        GitRemote& operator=(GitRemote&& o) noexcept {
            if (this != &o) {
                Clear();
                remote = o.remote;
                owned = o.owned;
                o.remote = nullptr;
                o.owned = false;
            }
            return *this;
        }

        void Lookup(git_repository* repo, const std::string& name) {
            Clear();
            GitException::Check(git_remote_lookup(&remote, repo, name.c_str()));
            owned = true;
        }

        void Create(git_repository* repo, const std::string& name, const std::string& url) {
            Clear();
            GitException::Check(git_remote_create(&remote, repo, name.c_str(), url.c_str()));
            owned = true;
        }

        void Adopt(git_remote* r, bool take_ownership = true) {
            Clear();
            remote = r;
            owned = take_ownership;
        }

        void Clear() {
            if (remote && owned) {
                git_remote_free(remote);
            }
            remote = nullptr;
            owned = false;
        }

        bool IsValid() const { return remote != nullptr; }
        git_remote* GetRaw() const { return remote; }

        std::string Name() const {
            if (!remote) return "";
            const char* n = git_remote_name(remote);
            return n ? n : "";
        }

        std::string Url() const {
            if (!remote) return "";
            const char* u = git_remote_url(remote);
            return u ? u : "";
        }
    };

    class GitBranchIterator {
    private:
        git_branch_iterator* iter = nullptr;
        bool owned = false;

    public:
        GitBranchIterator() = default;
        ~GitBranchIterator() { Clear(); }

        GitBranchIterator(const GitBranchIterator&) = delete;
        GitBranchIterator& operator=(const GitBranchIterator&) = delete;

        GitBranchIterator(GitBranchIterator&& o) noexcept
            : iter(o.iter), owned(o.owned) {
            o.iter = nullptr;
            o.owned = false;
        }

        GitBranchIterator& operator=(GitBranchIterator&& o) noexcept {
            if (this != &o) {
                Clear();
                iter = o.iter;
                owned = o.owned;
                o.iter = nullptr;
                o.owned = false;
            }
            return *this;
        }

        void Create(git_repository* repo, git_branch_t type = GIT_BRANCH_ALL) {
            Clear();
            GitException::Check(git_branch_iterator_new(&iter, repo, type));
            owned = true;
        }

        void Clear() {
            if (iter && owned) {
                git_branch_iterator_free(iter);
            }
            iter = nullptr;
            owned = false;
        }

        bool IsValid() const { return iter != nullptr; }
        git_branch_iterator* GetRaw() const { return iter; }

        bool Next(git_reference** out_ref, git_branch_t* out_type) {
            if (!iter) return false;
            return git_branch_next(out_ref, out_type, iter) == 0;
        }
    };

    class GitStatusList {
    private:
        git_status_list* status = nullptr;
        size_t count = 0;

    public:
        GitStatusList() = default;

        GitStatusList(git_repository* repo, unsigned int flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED) {
            Refresh(repo, flags);
        }

        ~GitStatusList() { Clear(); }

        void Refresh(git_repository* repo, unsigned int flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED) {
            Clear();
            git_status_options opts = GIT_STATUS_OPTIONS_INIT;
            opts.flags = flags;

            GitException::Check(git_status_list_new(&status, repo, &opts));
            count = status ? git_status_list_entrycount(status) : 0;
        }

        size_t Count() const { return count; }

        const git_status_entry* Get(size_t index) const {
            if (!status || index >= count) return nullptr;
            return git_status_byindex(status, index);
        }

        bool IsModified() const { return count > 0; }

        void Clear() {
            if (status) {
                git_status_list_free(status);
                status = nullptr;
                count = 0;
            }
        }

        GitStatusList(const GitStatusList&) = delete;
        GitStatusList& operator=(const GitStatusList&) = delete;

        GitStatusList(GitStatusList&& other) noexcept
            : status(other.status), count(other.count) {
            other.status = nullptr;
            other.count = 0;
        }

        GitStatusList& operator=(GitStatusList&& other) noexcept {
            if (this != &other) {
                Clear();
                status = other.status;
                count = other.count;
                other.status = nullptr;
                other.count = 0;
            }
            return *this;
        }
    };

    class GitCommit {
    private:
        git_commit* commit = nullptr;
        git_repository* repo = nullptr;
        bool owned = false;

    public:
        GitCommit() = default;

        GitCommit(git_repository* repo, const git_oid* oid) {
            Load(repo, oid);
        }

        GitCommit(git_repository* repo, const std::string& oid_str) {
            Load(repo, oid_str);
        }

        ~GitCommit() { Clear(); }

        void Load(git_repository* repo, const git_oid* oid) {
            Clear();
            this->repo = repo;
            GitException::Check(git_commit_lookup(&commit, repo, oid));
            owned = true;
        }

        void Load(git_repository* repo, const std::string& hash) {
            git_oid oid;
            GitException::Check(git_oid_fromstr(&oid, hash.c_str()));
            Load(repo, &oid);
        }

        GitCommitInfo GetInfo() const {
            if (!commit) throw GitException("Commit not loaded");

            GitCommitInfo info;

            char oid_str[GIT_OID_HEXSZ + 1];
            git_oid_fmt(oid_str, git_commit_id(commit));
            oid_str[GIT_OID_HEXSZ] = '\0';
            info.id = oid_str;

            info.message = git_commit_message(commit) ? git_commit_message(commit) : "";

            const git_signature* author = git_commit_author(commit);
            if (author) {
                info.author.name = author->name ? author->name : "";
                info.author.email = author->email ? author->email : "";
                info.author.time = author->when.time;
                info.author.offset = author->when.offset;
            }

            const git_signature* committer = git_commit_committer(commit);
            if (committer) {
                info.committer.name = committer->name ? committer->name : "";
                info.committer.email = committer->email ? committer->email : "";
                info.committer.time = committer->when.time;
                info.committer.offset = committer->when.offset;
            }

            info.commit_time = git_commit_time(commit);

            size_t parent_count = git_commit_parentcount(commit);
            for (size_t i = 0; i < parent_count; ++i) {
                const git_oid* parent_oid = git_commit_parent_id(commit, i);
                char parent_str[GIT_OID_HEXSZ + 1];
                git_oid_fmt(parent_str, parent_oid);
                parent_str[GIT_OID_HEXSZ] = '\0';
                info.parents.push_back(parent_str);
            }

            return info;
        }

        std::string GetId() const {
            if (!commit) return "";
            char oid_str[GIT_OID_HEXSZ + 1];
            git_oid_fmt(oid_str, git_commit_id(commit));
            oid_str[GIT_OID_HEXSZ] = '\0';
            return oid_str;
        }

        git_commit* GetRaw() const { return commit; }
        bool IsValid() const { return commit != nullptr; }

        void Clear() {
            if (commit && owned) {
                git_commit_free(commit);
            }
            commit = nullptr;
            repo = nullptr;
            owned = false;
        }

        GitCommit(const GitCommit&) = delete;
        GitCommit& operator=(const GitCommit&) = delete;

        GitCommit(GitCommit&& other) noexcept
            : commit(other.commit), repo(other.repo), owned(other.owned) {
            other.commit = nullptr;
            other.repo = nullptr;
            other.owned = false;
        }

        GitCommit& operator=(GitCommit&& other) noexcept {
            if (this != &other) {
                Clear();
                commit = other.commit;
                repo = other.repo;
                owned = other.owned;
                other.commit = nullptr;
                other.repo = nullptr;
                other.owned = false;
            }
            return *this;
        }
    };

    class GitTree {
        git_tree* tree = nullptr;
        bool owned = true;

    public:
        GitTree() = default;
        ~GitTree() { Clear(); }

        GitTree(const GitTree&) = delete;
        GitTree& operator=(const GitTree&) = delete;

        GitTree(GitTree&& o) noexcept : tree(o.tree), owned(o.owned) {
            o.tree = nullptr;
            o.owned = false;
        }
        GitTree& operator=(GitTree&& o) noexcept {
            if (this != &o) {
                Clear();
                tree = o.tree;
                owned = o.owned;
                o.tree = nullptr;
                o.owned = false;
            }
            return *this;
        }

        void Load(git_repository* repo, const git_oid* oid) {
            Clear();
            GitException::Check(git_tree_lookup(&tree, repo, oid));
            owned = true;
        }

        void LoadFromCommit(git_commit* commit) {
            Clear();
            GitException::Check(git_commit_tree(&tree, commit));
            owned = true;
        }

        void Clear() {
            if (tree && owned) {
                git_tree_free(tree);
            }
            tree = nullptr;
            owned = false;
        }

        bool IsValid() const { return tree != nullptr; }
        git_tree* GetRaw() const { return tree; }
        const git_tree* GetRawConst() const { return tree; }
    };

    class GitIndex {
        git_index* index = nullptr;

    public:
        GitIndex() = default;
        ~GitIndex() { Clear(); }

        GitIndex(const GitIndex&) = delete;
        GitIndex& operator=(const GitIndex&) = delete;

        GitIndex(GitIndex&& o) noexcept : index(o.index) { o.index = nullptr; }
        GitIndex& operator=(GitIndex&& o) noexcept {
            if (this != &o) {
                Clear();
                index = o.index;
                o.index = nullptr;
            }
            return *this;
        }

        void Load(git_repository* repo) {
            Clear();
            GitException::Check(git_repository_index(&index, repo));
        }

        void Clear() {
            if (index) {
                git_index_free(index);
                index = nullptr;
            }
        }

        bool IsValid() const { return index != nullptr; }
        git_index* GetRaw() const { return index; }

        void AddAll(unsigned int flags = GIT_INDEX_ADD_DEFAULT) {
            if (!index) throw GitException("Index not loaded");
            GitException::Check(git_index_add_all(index, nullptr, flags, nullptr, nullptr));
        }

        void WriteTree(git_oid* out_tree_oid) {
            if (!index) throw GitException("Index not loaded");
            GitException::Check(git_index_write_tree(out_tree_oid, index));
        }

        void Write() {
            if (!index) throw GitException("Index not loaded");
            GitException::Check(git_index_write(index));
        }

        void ClearEntries() {
            if (!index) throw GitException("Index not loaded");
            GitException::Check(git_index_clear(index));
        }

        void ReadTree(const GitTree& tree) {
            if (!index) throw GitException("Index not loaded");
            if (!tree.IsValid()) throw GitException("Tree not loaded");
            GitException::Check(git_index_read_tree(index, tree.GetRawConst()));
        }
    };

    class GitReference {
    private:
        git_reference* ref = nullptr;
        bool owned = true;

    public:
        GitReference() = default;
        ~GitReference() { Clear(); }

        GitReference(const GitReference&) = delete;
        GitReference& operator=(const GitReference&) = delete;

        GitReference(GitReference&& o) noexcept
            : ref(o.ref), owned(o.owned) {
            o.ref = nullptr;
            o.owned = false;
        }

        GitReference& operator=(GitReference&& o) noexcept {
            if (this != &o) {
                Clear();
                ref = o.ref;
                owned = o.owned;
                o.ref = nullptr;
                o.owned = false;
            }
            return *this;
        }

        void Lookup(git_repository* repo, const std::string& name) {
            Clear();
            GitException::Check(git_reference_lookup(&ref, repo, name.c_str()));
            owned = true;
        }

        void Head(git_repository* repo) {
            Clear();
            GitException::Check(git_repository_head(&ref, repo));
            owned = true;
        }

        void AdoptDup(git_reference* r) {
            Clear();
            if (r) {
                GitException::Check(git_reference_dup(&ref, r));
                owned = true;
            }
        }

        void Adopt(git_reference* r) {
            Clear();
            ref = r;
            owned = true;
        }

        void Clear() {
            if (ref && owned) {
                git_reference_free(ref);
            }
            ref = nullptr;
            owned = false;
        }

        bool IsValid() const { return ref != nullptr; }
        git_reference* GetRaw() const { return ref; }

        std::string Shorthand() const {
            if (!ref) return "";
            const char* s = git_reference_shorthand(ref);
            return s ? s : "";
        }

        std::string Name() const {
            if (!ref) return "";
            const char* s = git_reference_name(ref);
            return s ? s : "";
        }

        const git_oid* Target() const {
            return ref ? git_reference_target(ref) : nullptr;
        }

        bool IsBranch() const {
            return ref && git_reference_is_branch(ref) == 1;
        }

        bool IsHead() const {
            return ref && git_branch_is_head(ref) == 1;
        }
    };

    class GitBranch {
    private:
        GitReference ref;
        git_repository* repo = nullptr;
        git_branch_t type = GIT_BRANCH_LOCAL;

    public:
        GitBranch() = default;
        ~GitBranch() { Clear(); }

        GitBranch(const GitBranch&) = delete;
        GitBranch& operator=(const GitBranch&) = delete;

        GitBranch(GitBranch&& o) noexcept
            : ref(std::move(o.ref)), repo(o.repo), type(o.type) {
            o.repo = nullptr;
        }

        GitBranch& operator=(GitBranch&& o) noexcept {
            if (this != &o) {
                Clear();
                ref = std::move(o.ref);
                repo = o.repo;
                type = o.type;
                o.repo = nullptr;
            }
            return *this;
        }

        void CreateFromHead(git_repository* r, const std::string& branch_name,
            bool force = false) {
            if (!r) throw GitException("Repository is null");

            git_object* obj = nullptr;
            GitException::Check(git_revparse_single(&obj, r, "HEAD"));

            git_commit* head_commit = nullptr;
            GitException::Check(git_object_peel(
                reinterpret_cast<git_object**>(&head_commit), obj, GIT_OBJECT_COMMIT));
            git_object_free(obj);

            git_reference* new_ref = nullptr;
            int err = git_branch_create(&new_ref, r, branch_name.c_str(),
                head_commit, force ? 1 : 0);
            git_commit_free(head_commit);
            GitException::Check(err);

            Clear();
            repo = r;
            type = GIT_BRANCH_LOCAL;
            ref.Adopt(new_ref);
        }

        void CreateFromCommit(git_repository* r, const std::string& branch_name,
            const GitCommit& commit, bool force = false) {
            if (!r) throw GitException("Repository is null");
            if (!commit.GetRaw()) throw GitException("Commit is null");

            git_reference* new_ref = nullptr;
            GitException::Check(git_branch_create(&new_ref, r, branch_name.c_str(),
                commit.GetRaw(), force ? 1 : 0));

            Clear();
            repo = r;
            type = GIT_BRANCH_LOCAL;
            ref.Adopt(new_ref);
        }

        void CreateFromOid(git_repository* r, const std::string& branch_name,
            const git_oid* oid, bool force = false) {
            if (!r) throw GitException("Repository is null");
            if (!oid) throw GitException("Oid is null");

            git_commit* commit = nullptr;
            GitException::Check(git_commit_lookup(&commit, r, oid));

            git_reference* new_ref = nullptr;
            int err = git_branch_create(&new_ref, r, branch_name.c_str(),
                commit, force ? 1 : 0);
            git_commit_free(commit);
            GitException::Check(err);

            Clear();
            repo = r;
            type = GIT_BRANCH_LOCAL;
            ref.Adopt(new_ref);
        }

        void CreateFromCurrent(const std::string& branch_name, bool force = false) {
            if (!ref.IsValid()) throw GitException("Branch not loaded");
            if (!repo) throw GitException("Repository is null");

            git_repository* saved_repo = repo;
            const git_oid* oid = ref.Target();
            if (!oid) throw GitException("Current branch has no target");

            git_commit* commit = nullptr;
            GitException::Check(git_commit_lookup(&commit, saved_repo, oid));

            git_reference* new_ref = nullptr;
            int err = git_branch_create(&new_ref, saved_repo, branch_name.c_str(), commit, force);
            git_commit_free(commit);
            GitException::Check(err);

            Clear();
            repo = saved_repo;
            type = GIT_BRANCH_LOCAL;
            ref.Adopt(new_ref);
        }

        void LoadHead(git_repository* r) {
            Clear();
            repo = r;
            type = GIT_BRANCH_LOCAL;

            git_reference* head_ref = nullptr;
            GitException::Check(git_repository_head(&head_ref, r));
            ref.Adopt(head_ref);
        }

        void Load(git_repository* r, const std::string& name,
            git_branch_t branch_type = GIT_BRANCH_LOCAL) {
            Clear();
            repo = r;
            type = branch_type;

            git_reference* raw = nullptr;
            GitException::Check(git_branch_lookup(&raw, r, name.c_str(), branch_type));
            ref.Adopt(raw);
        }

        void LoadDup(git_repository* r, git_reference* raw_ref,
            git_branch_t branch_type = GIT_BRANCH_LOCAL) {
            Clear();
            repo = r;
            type = branch_type;
            ref.AdoptDup(raw_ref);
        }

        std::string CreateCommit(const std::string& message,
            const std::string& author_name,
            const std::string& author_email,
            const std::string& committer_name = "",
            const std::string& committer_email = "") {
            if (!ref.IsValid()) throw GitException("Branch not loaded");

            GitIndex index;
            index.Load(repo);
            index.AddAll(GIT_INDEX_ADD_DEFAULT);

            git_oid tree_oid;
            index.WriteTree(&tree_oid);
            index.Clear();

            GitTree tree;
            tree.Load(repo, &tree_oid);

            const std::string cname = committer_name.empty() ? author_name : committer_name;
            const std::string cemail = committer_email.empty() ? author_email : committer_email;

            GitSignatureObj author_sig;
            author_sig.Now(author_name, author_email);

            GitSignatureObj committer_sig;
            committer_sig.Now(cname, cemail);

            GitCommit head = HeadCommit();

            const git_commit* parents[1] = { nullptr };
            int parent_count = 0;

            if (head.IsValid()) {
                parents[0] = head.GetRaw();
                parent_count = 1;
            }

            git_oid commit_oid;
            GitException::Check(git_commit_create(&commit_oid, repo, "HEAD",
                author_sig.GetRaw(), committer_sig.GetRaw(),
                nullptr, message.c_str(),
                tree.GetRaw(), parent_count, parents));

            char oid_str[GIT_OID_HEXSZ + 1];
            git_oid_fmt(oid_str, &commit_oid);
            oid_str[GIT_OID_HEXSZ] = '\0';
            return oid_str;
        }

        std::string GetName() const {
            if (!ref.IsValid()) return "";
            const char* name = nullptr;
            git_branch_name(&name, ref.GetRaw());
            return name ? name : "";
        }

        std::string GetFullName() const {
            return ref.IsValid() ? ref.Name() : "";
        }

        GitCommit HeadCommit() const {
            if (!ref.IsValid()) throw GitException("Branch not loaded");

            const git_oid* oid = ref.Target();
            if (!oid) throw GitException("Branch has no target");

            return GitCommit(repo, oid);
        }

        bool IsHead() const { return ref.IsHead(); }
        git_branch_t GetType() const { return type; }
        git_reference* GetRaw() const { return ref.GetRaw(); }
        bool IsValid() const { return ref.IsValid(); }

        void Clear() {
            ref.Clear();
            repo = nullptr;
        }
    };

    class GitSignatureObj {
        git_signature* sig = nullptr;

    public:
        GitSignatureObj() = default;
        ~GitSignatureObj() { Clear(); }

        GitSignatureObj(const GitSignatureObj&) = delete;
        GitSignatureObj& operator=(const GitSignatureObj&) = delete;

        GitSignatureObj(GitSignatureObj&& o) noexcept : sig(o.sig) { o.sig = nullptr; }
        GitSignatureObj& operator=(GitSignatureObj&& o) noexcept {
            if (this != &o) {
                Clear();
                sig = o.sig;
                o.sig = nullptr;
            }
            return *this;
        }

        void Create(const std::string& name, const std::string& email,
            time_t when = 0, int offset = 0) {
            Clear();
            if (when == 0) when = time(nullptr);
            GitException::Check(git_signature_new(&sig, name.c_str(), email.c_str(), when, offset));
        }

        void Now(const std::string& name, const std::string& email) {
            Create(name, email, time(nullptr), 0);
        }

        void Clear() {
            if (sig) {
                git_signature_free(sig);
                sig = nullptr;
            }
        }

        bool IsValid() const { return sig != nullptr; }
        git_signature* GetRaw() const { return sig; }
    };

    struct ProgressState {
        int last_percent = -1;

        void OnProgress(const std::string& progress) {
            std::cout << "[PROGRESS] " << progress << std::flush;
        }

        void OnTransfer(int received_objects, int total_objects, int indexed_objects) {
            if (total_objects <= 0) return;

            int percent = (received_objects * 100) / total_objects;
            if (percent != last_percent) {
                last_percent = percent;
                std::cout << "\rReceiving objects: " << percent << "% ("
                    << received_objects << "/" << total_objects << "), "
                    << indexed_objects << " indexed" << std::flush;
            }
        }

        void Reset() { last_percent = -1; }
    };

    class GitRepository {
    private:
        struct CallbackPayload {
            ProgressState* progress = nullptr;
            GitCredentials credentials;
        };

        CallbackPayload cb_payload;

        git_repository* repo = nullptr;
        std::string directory;
        bool isOpen = false;

        std::vector<GitBranch> branches_;
        GitBranch current_branch_;
        bool branches_loaded_ = false;

        static int CredentialsCallback(
            git_cred** out,
            const char* /*url*/,
            const char* username_from_url,
            unsigned int allowed_types, void* payload)
        {
            if (!payload) return GIT_EAUTH;

            CallbackPayload* context = static_cast<CallbackPayload*>(payload);
            if (!context) return GIT_EAUTH;

            auto* creds = &context->credentials;

            if (creds->attempt_count > 0) {
                return GIT_EAUTH;
            }
            creds->attempt_count++;

            if (creds->type == GitCredentials::Type::TOKEN &&
                (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT))
            {
                if (creds->token.empty()) return GIT_EAUTH;

                std::string username = creds->username.empty()
                    ? (username_from_url ? username_from_url : "x-access-token")
                    : creds->username;

                return git_cred_userpass_plaintext_new(out, username.c_str(), creds->token.c_str());
            }

            if (creds->type == GitCredentials::Type::SSH_KEY &&
                (allowed_types & GIT_CREDENTIAL_SSH_KEY))
            {
                if (creds->ssh_private_key_path.empty()) return GIT_EAUTH;

                return git_cred_ssh_key_new(out,
                    creds->username.empty() ? "git" : creds->username.c_str(),
                    creds->ssh_public_key_path.empty() ? nullptr : creds->ssh_public_key_path.c_str(),
                    creds->ssh_private_key_path.c_str(),
                    creds->ssh_passphrase.empty() ? nullptr : creds->ssh_passphrase.c_str());
            }

            return GIT_PASSTHROUGH;
        }

        static int ProgressCallback(const char* str, int len, void* payload) {
            if (!payload || !str || len <= 0) return 0;
            auto* context = static_cast<CallbackPayload*>(payload);
            if (context && context->progress) {
                context->progress->OnProgress(std::string(str, len));
            }
            return 0;
        }

        static int TransferProgressCallback(const git_transfer_progress* stats, void* payload) {
            if (!payload || !stats) return 0;
            auto* context = static_cast<CallbackPayload*>(payload);
            if (context && context->progress) {
                context->progress->OnTransfer(
                    stats->received_objects,
                    stats->total_objects,
                    stats->indexed_objects);
            }
            return 0;
        }

        void InitialCallbacks(git_remote_callbacks& callbacks, ProgressState* progress) {
            cb_payload.progress = progress;
            cb_payload.credentials.attempt_count = 0;

            callbacks = GIT_REMOTE_CALLBACKS_INIT;
            callbacks.payload = &cb_payload;
            callbacks.credentials = CredentialsCallback;
            callbacks.sideband_progress = ProgressCallback;
            callbacks.transfer_progress = TransferProgressCallback;
            callbacks.certificate_check = [](git_cert*, int valid, const char*, void*) -> int {
                return valid ? 0 : 1;
                };
        }

        void Close() {
            branches_.clear();
            current_branch_.Clear();
            branches_loaded_ = false;

            if (repo) {
                git_repository_free(repo);
                repo = nullptr;
                isOpen = false;
                directory.clear();
            }
        }

    public:
        GitRepository() = default;

        explicit GitRepository(const std::string& path) {
            Open(path);
        }

        ~GitRepository() {
            Close();
        }

        void Init(const std::string& path, bool bare = false) {
            Close();
            directory = path;
            GitException::Check(git_repository_init(&repo, directory.c_str(), bare));
            isOpen = true;
        }

        void Open(const std::string& path) {
            Close();
            directory = path;
            GitException::Check(git_repository_open(&repo, path.c_str()));
            isOpen = true;
        }

        void Clone(const std::string& path, const std::string& url, const git_clone_options& opts) {
            Close();
            directory = path;
            GitException::Check(git_clone(&repo, url.c_str(), directory.c_str(), &opts));
            isOpen = true;
        }

        void SetCredentials(const GitCredentials& creds) {
            cb_payload.credentials = creds;
            cb_payload.credentials.attempt_count = 0;
        }

        void Clone(const std::string& url, const std::string& path,
            ProgressState* progress = nullptr)
        {
            git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
            InitialCallbacks(opts.fetch_opts.callbacks, progress);
            Clone(path, url, opts);
        }

        void RefreshBranches(git_branch_t type = GIT_BRANCH_ALL) {
            if (!isOpen) throw GitException("Repository not open");

            branches_.clear();

            GitBranchIterator iter;
            iter.Create(repo, type);

            git_reference* raw_ref = nullptr;
            git_branch_t branch_type;

            while (iter.Next(&raw_ref, &branch_type)) {
                try {
                    GitBranch branch;
                    branch.LoadDup(repo, raw_ref, branch_type);
                    branches_.push_back(std::move(branch));
                }
                catch (const GitException&) {
                    // пропускаем сломанные ветки
                }
                git_reference_free(raw_ref);
                raw_ref = nullptr;
            }

            branches_loaded_ = true;
        }

        void RefreshCurrentBranch() {
            if (!isOpen) throw GitException("Repository not open");

            current_branch_.Clear();

            git_reference* head_ref = nullptr;
            int err = git_repository_head(&head_ref, repo);
            if (err == GIT_EUNBORNBRANCH || err == GIT_ENOTFOUND) {
                // пустой репозиторий — текущей ветки нет
                return;
            }
            GitException::Check(err);

            current_branch_.LoadDup(repo, head_ref, GIT_BRANCH_LOCAL);
            git_reference_free(head_ref);
        }

        void RefreshAll(git_branch_t type = GIT_BRANCH_ALL) {
            RefreshBranches(type);
            RefreshCurrentBranch();
        }

        const std::vector<GitBranch>& GetBranchesCached() const {
            return branches_;
        }

        const GitBranch& GetCurrentBranchCached() const {
            return current_branch_;
        }

        bool AreBranchesLoaded() const { return branches_loaded_; }

        GitBranch GetBranch(const std::string& name, git_branch_t type = GIT_BRANCH_LOCAL) {
            if (!isOpen) throw GitException("Repository not open");
            GitBranch branch;
            branch.Load(repo, name, type);
            return branch;
        }

        GitBranch GetHeadBranch() {
            if (!isOpen) throw GitException("Repository not open");
            GitBranch branch;
            branch.LoadHead(repo);
            return branch;
        }

        std::vector<GitBranch> GetBranches(git_branch_t type = GIT_BRANCH_ALL) {
            if (!isOpen) throw GitException("Repository not open");

            std::vector<GitBranch> branches;
            GitBranchIterator iter;
            iter.Create(repo, type);

            git_reference* raw_ref = nullptr;
            git_branch_t branch_type;

            while (iter.Next(&raw_ref, &branch_type)) {
                try {
                    GitBranch branch;
                    branch.LoadDup(repo, raw_ref, branch_type);
                    branches.push_back(std::move(branch));
                }
                catch (const GitException&) {
                    // пропускаем
                }
                git_reference_free(raw_ref);
                raw_ref = nullptr;
            }

            return branches;
        }

        GitBranch CreateBranchFromHead(const std::string& branch_name,
            bool force = false) {
            if (!isOpen) throw GitException("Repository not open");

            GitBranch branch;
            branch.CreateFromHead(repo, branch_name, force);
            RefreshBranches();
            return branch;
        }

        GitBranch CreateBranchFromCommit(const std::string& branch_name,
            const GitCommit& commit, bool force = false) {
            if (!isOpen) throw GitException("Repository not open");

            GitBranch branch;
            branch.CreateFromCommit(repo, branch_name, commit, force);
            RefreshBranches();
            return branch;
        }

        GitBranch CreateBranchFromOid(const std::string& branch_name, const git_oid* oid, bool force = false) {
            if (!isOpen) throw GitException("Repository not open");
            GitBranch branch;
            branch.CreateFromOid(repo, branch_name, oid, force);
            RefreshBranches();
            return branch;
        }

        GitBranch CreateBranchFromOid(const std::string& branch_name, const std::string& hash_commit, bool force = false) {
            if (!isOpen) throw GitException("Repository not open");
            git_oid oid;
            git_oid_fromstr(&oid, hash_commit.c_str());
            return CreateBranchFromOid(branch_name, &oid, force);
        }

        GitBranch CreateBranchFromCurrent(const GitBranch& current,
            const std::string& branch_name, bool force = false) {
            if (!isOpen) throw GitException("Repository not open");
            if (!current.IsValid()) throw GitException("Current branch is not valid");

            GitBranch branch;
            branch.LoadDup(repo, current.GetRaw(),
                static_cast<git_branch_t>(current.GetType()));
            branch.CreateFromCurrent(branch_name, force);
            RefreshBranches();
            return branch;
        }

        void PushBranch(const std::string& branch_name,
            const std::string& remote_name = "origin",
            ProgressState* progress = nullptr,
            bool set_upstream = false)
        {
            if (!isOpen) throw GitException("Repository not open");
            if (branch_name.empty()) throw GitException("Branch name is empty");

            GitRemote remote;
            remote.Lookup(repo, remote_name);

            std::string local_ref = "refs/heads/" + branch_name;
            std::string remote_ref = "refs/heads/" + branch_name;
            std::string refspec = local_ref + ":" + remote_ref;

            git_push_options opts = GIT_PUSH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks, progress);

            const char* refspec_ptr = refspec.c_str();
            git_strarray refspecs;
            refspecs.count = 1;
            refspecs.strings = (char**)&refspec_ptr;

            GitException::Check(git_remote_push(remote.GetRaw(), &refspecs, &opts));

            if (set_upstream) {
                SetUpstream(branch_name, remote_name);
            }
        }

        // Запушить несколько веток по именам
        void PushBranches(const std::vector<std::string>& branch_names,
            const std::string& remote_name = "origin",
            ProgressState* progress = nullptr)
        {
            if (!isOpen) throw GitException("Repository not open");
            if (branch_names.empty()) throw GitException("Branch list is empty");

            GitRemote remote;
            remote.Lookup(repo, remote_name);

            std::vector<std::string> refspec_storage;
            refspec_storage.reserve(branch_names.size());

            for (const auto& b : branch_names) {
                if (b.empty()) continue;
                refspec_storage.push_back("refs/heads/" + b + ":refs/heads/" + b);
            }

            if (refspec_storage.empty()) throw GitException("No valid branches to push");

            std::vector<const char*> refspec_ptrs;
            refspec_ptrs.reserve(refspec_storage.size());
            for (const auto& s : refspec_storage) {
                refspec_ptrs.push_back(s.c_str());
            }

            git_push_options opts = GIT_PUSH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks, progress);

            git_strarray refspecs;
            refspecs.count = refspec_ptrs.size();
            refspecs.strings = const_cast<char**>(refspec_ptrs.data());

            GitException::Check(git_remote_push(remote.GetRaw(), &refspecs, &opts));
        }

        // Запушить все локальные ветки (кроме, опционально, указанных)
        void PushAllBranches(const std::string& remote_name = "origin",
            ProgressState* progress = nullptr,
            const std::vector<std::string>& exclude = {})
        {
            if (!isOpen) throw GitException("Repository not open");

            // Загружаем локальные ветки
            RefreshBranches(GIT_BRANCH_LOCAL);

            std::vector<std::string> names;
            for (const auto& b : branches_) {
                std::string name = b.GetName();
                if (name.empty()) continue;

                // Проверяем исключения
                bool skip = false;
                for (const auto& ex : exclude) {
                    if (name == ex) { skip = true; break; }
                }
                if (!skip) names.push_back(name);
            }

            if (names.empty()) {
                std::cout << "No local branches to push" << std::endl;
                return;
            }

            PushBranches(names, remote_name, progress);
        }

        void SetUpstream(const std::string& local_branch,
            const std::string& remote_name = "origin")
        {
            if (!isOpen) throw GitException("Repository not open");
            if (local_branch.empty()) throw GitException("Local branch name is empty");

            // Проверяем, что ветка существует локально
            git_reference* ref = nullptr;
            GitException::Check(git_branch_lookup(&ref, repo, local_branch.c_str(),
                GIT_BRANCH_LOCAL));
            git_reference_free(ref);

            git_config* cfg = nullptr;
            GitException::Check(git_repository_config(&cfg, repo));

            std::string remote_key = "branch." + local_branch + ".remote";
            std::string merge_key = "branch." + local_branch + ".merge";
            std::string merge_val = "refs/heads/" + local_branch;

            GitException::Check(git_config_set_string(cfg, remote_key.c_str(),
                remote_name.c_str()));
            GitException::Check(git_config_set_string(cfg, merge_key.c_str(),
                merge_val.c_str()));

            git_config_free(cfg);
        }

        // Хелпер: получить git_reference* ветки (нужно освободить)
        git_reference* GetBranchRaw(const std::string& branch_name) {
            git_reference* ref = nullptr;
            GitException::Check(git_branch_lookup(&ref, repo, branch_name.c_str(), GIT_BRANCH_LOCAL));
            return ref;
        }

        GitStatusList GetStatus(unsigned int flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED) {
            if (!isOpen) throw GitException("Repository not open");
            return GitStatusList(repo, flags);
        }

        GitCommit GetHeadCommit() {
            if (!isOpen) throw GitException("Repository not open");
            GitBranch branch = GetHeadBranch();
            return branch.HeadCommit();
        }

        std::string CreateCommit(const std::string& message,
            const std::string& author_name,
            const std::string& author_email,
            const std::string& committer_name = "",
            const std::string& committer_email = "") {
            if (!isOpen) throw GitException("Repository not open");
            std::string hash = GetHeadBranch().CreateCommit(
                message, author_name, author_email, committer_name, committer_email);
            RefreshAll();
            return hash;
        }

        std::vector<std::string> GetRemotes() const {
            if (!isOpen) throw GitException("Repository not open");

            GitStrArray remotes;
            remotes.Load(git_remote_list, repo);
            return remotes.ToVector();
        }

        void AddRemote(const std::string& name, const std::string& url) {
            if (!isOpen) throw GitException("Repository not open");
            GitRemote remote;
            remote.Create(repo, name, url);
        }

        void RemoveRemote(const std::string& name) {
            if (!isOpen) throw GitException("Repository not open");
            GitException::Check(git_remote_delete(repo, name.c_str()));
        }

        void Fetch(const std::string& remote_name = "origin",
            ProgressState* progress = nullptr)
        {
            if (!isOpen) throw GitException("Repository not open");

            GitRemote remote;
            remote.Lookup(repo, remote_name);

            git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks, progress);

            GitException::Check(git_remote_fetch(remote.GetRaw(), nullptr, &opts, nullptr));
        }

        void MergeFastForward(const std::string& remote_name = "origin") {
            if (!isOpen) throw GitException("Repository not open");

            GitReference head_ref;
            head_ref.Head(repo);

            std::string branch_name = head_ref.Shorthand();
            if (branch_name.empty()) throw GitException("No current branch");

            std::string remote_ref_name = "refs/remotes/" + remote_name + "/" + branch_name;
            GitReference remote_ref;
            remote_ref.Lookup(repo, remote_ref_name);

            const git_oid* head_oid = head_ref.Target();
            const git_oid* remote_oid = remote_ref.Target();
            if (!head_oid || !remote_oid) throw GitException("Missing oid");

            if (git_oid_equal(head_oid, remote_oid)) {
                std::cout << "Already up to date" << std::endl;
                return;
            }

            git_oid merge_base;
            GitException::Check(git_merge_base(&merge_base, repo, head_oid, remote_oid));

            if (!git_oid_equal(&merge_base, head_oid)) {
                throw GitException("Non-fast-forward merge not implemented");
            }

            std::cout << "Fast-forwarding..." << std::endl;

            GitCommit remote_commit(repo, remote_oid);
            GitTree remote_tree;
            remote_tree.LoadFromCommit(remote_commit.GetRaw());

            GitIndex index;
            index.Load(repo);
            index.ClearEntries();
            index.ReadTree(remote_tree);
            index.Write();

            git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
            checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE;
            GitException::Check(git_checkout_tree(
                repo, reinterpret_cast<const git_object*>(remote_tree.GetRaw()),
                &checkout_opts));

            git_reference* new_ref = nullptr;
            GitException::Check(git_reference_set_target(
                &new_ref, head_ref.GetRaw(), remote_oid, "Fast-forward merge"));
            if (new_ref) git_reference_free(new_ref);

            std::cout << "HEAD updated successfully" << std::endl;
            RefreshAll();
        }

        void Pull(const std::string& remote_name = "origin",
            ProgressState* progress = nullptr)
        {
            std::cout << "Fetching from " << remote_name << "..." << std::endl;
            Fetch(remote_name, progress);

            std::cout << "Merging changes..." << std::endl;
            MergeFastForward(remote_name);

            std::cout << "Pull completed successfully" << std::endl;
        }

        bool HasUncommittedChanges() {
            if (!isOpen) throw GitException("Repository not open");
            GitStatusList status;
            status.Refresh(repo,
                GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                GIT_STATUS_OPT_SORT_CASE_SENSITIVELY);
            return status.Count() > 0;
        }

        void Push(const std::string& remote_name = "origin",
            const std::string& branch = "",
            ProgressState* progress = nullptr)
        {
            if (!isOpen) throw GitException("Repository not open");

            GitRemote remote;
            remote.Lookup(repo, remote_name);

            std::string branch_name = branch;
            if (branch_name.empty()) {
                GitBranch head = GetHeadBranch();
                branch_name = head.GetName();
            }
            if (branch_name.empty()) {
                throw GitException("No branch specified and no current branch");
            }

            std::string branch_ref = "refs/heads/" + branch_name +
                ":refs/heads/" + branch_name;

            git_push_options opts = GIT_PUSH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks, progress);

            const char* refspec = branch_ref.c_str();
            git_strarray refspecs;
            refspecs.count = 1;
            refspecs.strings = (char**)&refspec;

            GitException::Check(git_remote_push(remote.GetRaw(), &refspecs, &opts));
        }

        std::string GetPath() const { return directory; }
        bool IsOpen() const { return isOpen; }

        bool IsBare() const {
            if (!isOpen) throw GitException("Repository not open");
            return git_repository_is_bare(repo) == 1;
        }

        bool IsEmpty() const {
            if (!isOpen) throw GitException("Repository not open");
            return git_repository_is_empty(repo) == 1;
        }

        std::string GetWorkdir() const {
            if (!isOpen) throw GitException("Repository not open");
            const char* workdir = git_repository_workdir(repo);
            return workdir ? workdir : "";
        }

        git_repository* GetRaw() const { return repo; }

        bool operator==(const GitRepository& other) const { return repo == other.repo; }
        bool operator!=(const GitRepository& other) const { return !(*this == other); }

        GitRepository(const GitRepository&) = delete;
        GitRepository& operator=(const GitRepository&) = delete;

        GitRepository(GitRepository&& other) noexcept
            : cb_payload(std::move(other.cb_payload)),
            repo(other.repo),
            directory(std::move(other.directory)),
            isOpen(other.isOpen),
            branches_(std::move(other.branches_)),
            current_branch_(std::move(other.current_branch_)),
            branches_loaded_(other.branches_loaded_)
        {
            other.repo = nullptr;
            other.isOpen = false;
            other.branches_loaded_ = false;
        }

        GitRepository& operator=(GitRepository&& other) noexcept {
            if (this != &other) {
                Close();
                cb_payload = std::move(other.cb_payload);
                repo = other.repo;
                directory = std::move(other.directory);
                isOpen = other.isOpen;
                branches_ = std::move(other.branches_);
                current_branch_ = std::move(other.current_branch_);
                branches_loaded_ = other.branches_loaded_;
                other.repo = nullptr;
                other.isOpen = false;
                other.branches_loaded_ = false;
            }
            return *this;
        }
    };

    class Git {
    private:
        GitVersion version;
        static inline bool isInitialized = false;

        void Initialize() {
            if (!isInitialized) {
                git_libgit2_init();
                isInitialized = true;

                int major, minor, patch;
                git_libgit2_version(&major, &minor, &patch);
                version.major = major;
                version.minor = minor;
                version.patch = patch;
            }
        }

    public:
        Git() { Initialize(); }

        ~Git() {
            if (isInitialized) {
                git_libgit2_shutdown();
                isInitialized = false;
            }
        }

        GitVersion GetVersion() const { return version; }
        std::string GetVersionString() const { return version.ToString(); }
        int GetFeatures() const { return git_libgit2_features(); }
        bool HasFeature(int feature) const { return (GetFeatures() & feature) != 0; }

        std::string GetFeaturesString() const {
            int features = GetFeatures();
            std::string result;

            if (features & GIT_FEATURE_HTTPS) result += "HTTPS ";
            if (features & GIT_FEATURE_SSH) result += "SSH ";
            if (features & GIT_FEATURE_SHA1) result += "SHA1 ";
            if (features & GIT_FEATURE_SHA256) result += "SHA256 ";

            if (result.empty()) result = "None";
            return result;
        }

        GitRepository OpenRepository(const std::string& path) {
            GitRepository repo;
            repo.Open(path);
            return repo;
        }

        GitRepository OpenRepository(const std::string& path, const GitCredentials& creds) {
            GitRepository repo;
            repo.Open(path);
            repo.SetCredentials(creds);
            return repo;
        }

        GitRepository InitRepository(const std::string& path, bool bare = false) {
            GitRepository repo;
            repo.Init(path, bare);
            return repo;
        }

        GitRepository CloneRepository(const std::string& url,
            const std::string& path,
            const GitCredentials& creds,
            ProgressState* progress = nullptr)
        {
            GitRepository repo;
            repo.SetCredentials(creds);
            repo.Clone(url, path, progress);
            return repo;
        }

        GitRepository CloneRepository(const std::string& url,
            const std::string& path,
            ProgressState* progress = nullptr)
        {
            return CloneRepository(url, path, GitCredentials::Anonymous(), progress);
        }
    };

} // namespace GitWrapper

int main() {

    setlocale(LC_ALL, "Russian");

    try {
        GitWrapper::Git git;

        auto token_creds = GitWrapper::GitCredentials::Token(USER_NAME, USER_GHP_TOKEN);

        GitWrapper::ProgressState ps_clone;

        auto repository_test = git.CloneRepository(
            USER_URL_REPO,
            "./repo_token",
            token_creds,
            &ps_clone
        );

        std::cout << "Path: " << repository_test.GetPath() << std::endl;

        // Обновляем кэш веток
        repository_test.RefreshAll();

        std::cout << "Branches loaded: "
            << repository_test.GetBranchesCached().size() << "\n";

        for (const auto& b : repository_test.GetBranchesCached()) {
            std::cout << "  branch: " << b.GetName() << " (head=" << (b.IsHead() ? "yes" : "no") << ")\n";
        }

        const auto& cur = repository_test.GetCurrentBranchCached();
        if (cur.IsValid()) {
            std::cout << "Current branch: " << cur.GetName() << "\n";
        }

        auto main_branch = repository_test.GetBranch("main");
        std::cout << "main is head ? " << (main_branch.IsHead() ? "YES" : "NO") << "\n";

        auto head_commit = main_branch.HeadCommit();
        auto head_info = head_commit.GetInfo();

        std::cout << "Author: " << head_info.author.name  << " <" << head_info.author.email << ">\n";
        std::cout << "Message: " << Utf8ToWindows1251(head_info.message) << "\n";

        std::string hash_commit = main_branch.CreateCommit(
            "Коммит #" + make_random_name(),
            head_info.author.name,
            head_info.author.email);

        std::cout << "Hash commit: " << hash_commit << "\n";
        repository_test.RefreshAll();

        // Push
        GitWrapper::ProgressState ps_push;
        std::cout << "Pushing to origin..." << std::endl;
        repository_test.Push("origin", "", &ps_push);
        std::cout << "\nCommit pushed!" << "\n";

        std::string new_branch_head_name = "feature/from-head-" + make_random_name();
        std::string new_branch_commit_name = "feature/from-commit-" + make_random_name();
        std::string new_branch_oid_name = "feature/from-oid-" + make_random_name();
        std::string new_branch_current_name = "feature/from-current-" + make_random_name();

        auto branch_from_head = repository_test.CreateBranchFromHead(new_branch_head_name);
        std::cout << "Created branch from HEAD: " << branch_from_head.GetName() << "\n";

        auto head_commit2 = repository_test.GetHeadCommit();
        auto branch_from_commit = repository_test.CreateBranchFromCommit(new_branch_commit_name, head_commit2);
        std::cout << "Created branch from commit: " << branch_from_commit.GetName() << "\n";

        auto branch_from_oid = repository_test.CreateBranchFromOid(new_branch_oid_name, hash_commit);
        std::cout << "Created branch from oid: " << branch_from_oid.GetName() << "\n";

        auto branch_from_current = repository_test.CreateBranchFromCurrent(main_branch, new_branch_current_name);
        std::cout << "Created branch from current: " << branch_from_current.GetName() << "\n";

        std::cout << "\nAll branches in cache:\n";
        for (const auto& b : repository_test.GetBranchesCached()) {
            std::cout << "  " << b.GetName() << "\n";
        }

        repository_test.PushBranch(new_branch_head_name, "origin", &ps_push, true);
        std::cout << "\nPushed: " << new_branch_head_name << "\n";

        repository_test.PushBranch(new_branch_commit_name, "origin", &ps_push, true);
        std::cout << "\nPushed: " << new_branch_commit_name << "\n";
        
        std::vector<std::string> branches_to_push = {
            new_branch_oid_name,
            new_branch_current_name
        };
        repository_test.PushBranches(branches_to_push, "origin", &ps_push);
        std::cout << "\nPushed list of branches\n";

        std::cout << "\n=== Pushing all local branches except main ===\n";
        repository_test.PushAllBranches("origin", &ps_push, { "main" });
        std::cout << "\n=== Done ===\n";

        GitWrapper::ProgressState ps_pull;
        repository_test.Pull("origin", &ps_pull);

    }
    catch (const GitWrapper::GitException& e) {
        std::cerr << "Error: " << e.what() << std::endl;

        const git_error* err = git_error_last();
        if (err && err->message) {
            std::cerr << "Git error details: " << err->message << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Std error: " << e.what() << std::endl;
    }

    return 0;
}