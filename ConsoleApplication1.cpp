
#include "FileName.h"
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

// FIX: inline
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

    class GitStatusList {
    private:
        git_status_list* status = nullptr;
        size_t count = 0;

    public:
        GitStatusList() = default;

        GitStatusList(git_repository* repo, unsigned int flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED) {
            Refresh(repo, flags);
        }

        ~GitStatusList() {
            Clear();
        }

        void Refresh(git_repository* repo, unsigned int flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED) {
            Clear();
            git_status_options opts = GIT_STATUS_OPTIONS_INIT;
            opts.flags = flags;

            int error = git_status_list_new(&status, repo, &opts);
            GitException::Check(error);

            count = status ? git_status_list_entrycount(status) : 0;
        }

        size_t Count() const {
            return count;
        }

        const git_status_entry* Get(size_t index) const {
            if (!status || index >= count) return nullptr;
            return git_status_byindex(status, index);
        }

        std::string GetStatusString(const git_status_entry* entry) const {
            if (!entry) return "";

            std::string result;
            if (entry->status & GIT_STATUS_INDEX_NEW) result += "New (index) ";
            if (entry->status & GIT_STATUS_INDEX_MODIFIED) result += "Modified (index) ";
            if (entry->status & GIT_STATUS_INDEX_DELETED) result += "Deleted (index) ";
            if (entry->status & GIT_STATUS_INDEX_RENAMED) result += "Renamed (index) ";
            if (entry->status & GIT_STATUS_INDEX_TYPECHANGE) result += "Type change (index) ";

            if (entry->status & GIT_STATUS_WT_NEW) result += "New (working) ";
            if (entry->status & GIT_STATUS_WT_MODIFIED) result += "Modified (working) ";
            if (entry->status & GIT_STATUS_WT_DELETED) result += "Deleted (working) ";
            if (entry->status & GIT_STATUS_WT_RENAMED) result += "Renamed (working) ";
            if (entry->status & GIT_STATUS_WT_TYPECHANGE) result += "Type change (working) ";

            return result.empty() ? "Unchanged" : result;
        }

        bool IsModified() const {
            return count > 0;
        }

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

        ~GitCommit() {
            Clear();
        }

        void Load(git_repository* repo, const git_oid* oid) {
            Clear();
            this->repo = repo;
            int error = git_commit_lookup(&commit, repo, oid);
            GitException::Check(error);
            owned = true;
        }

        void Load(git_repository* repo, const std::string& hash) {
            git_oid oid;
            int error = git_oid_fromstr(&oid, hash.c_str());
            GitException::Check(error);
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

        git_commit* GetRaw() const {
            return commit;
        }

        bool IsValid() const {
            return commit != nullptr;
        }

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

        static GitCommit GetCommit(git_repository* repo, const std::string& hash) {
            return GitCommit(repo, hash);
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
        git_reference* ref = nullptr;
        bool owned = true;

    public:
        GitReference() = default;
        ~GitReference() { Clear(); }

        GitReference(const GitReference&) = delete;
        GitReference& operator=(const GitReference&) = delete;

        GitReference(GitReference&& o) noexcept : ref(o.ref), owned(o.owned) {
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

        void Adopt(git_reference* r, bool take_ownership = true) {
            Clear();
            ref = r;
            owned = take_ownership;
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
            if (!ref) return nullptr;
            return git_reference_target(ref);
        }

        bool IsBranch() const {
            return ref && git_reference_is_branch(ref) == 1;
        }

        bool IsHead() const {
            return ref && git_branch_is_head(ref) == 1;
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

    class GitBranch {
    private:
        git_reference* ref = nullptr;
        git_repository* repo = nullptr;
        git_branch_t type;
        bool owned = false;

        void Init(git_reference* branch_ref, git_repository* branch_repo,
            git_branch_t branch_type, bool branch_owned = true)
        {
            ref = branch_ref;
            repo = branch_repo;
            type = branch_type;
            owned = branch_owned;
        }

    public:
        GitBranch() : type(GIT_BRANCH_LOCAL) {}

        ~GitBranch() {
            Clear();
        }

        void Load(git_repository* repo, git_branch_t branch_type = GIT_BRANCH_LOCAL) {
            Clear();
            int error = git_repository_head(&ref, repo);
            GitException::Check(error);
            Init(ref, repo, branch_type);
        }

        void Load(git_repository* repo, const std::string& name,
            git_branch_t branch_type = GIT_BRANCH_LOCAL) {
            Clear();
            int error = git_branch_lookup(&ref, repo, name.c_str(), branch_type);
            GitException::Check(error);
            Init(ref, repo, branch_type);
        }

        void Load(git_repository* repo, git_reference* ref,
            git_branch_t branch_type = GIT_BRANCH_LOCAL) {
            Clear();
            Init(ref, repo, branch_type);
        }

        std::string CreateCommit(const std::string& message,
            const std::string& author_name,
            const std::string& author_email,
            const std::string& committer_name = "",
            const std::string& committer_email = "") {
            if (!ref) throw GitException("Branch not loaded");

            GitIndex index;
            index.Load(repo);
            index.AddAll(GIT_INDEX_ADD_DEFAULT);

            git_oid tree_oid;
            index.WriteTree(&tree_oid);
            index.Clear();

            GitTree tree;
            tree.Load(repo, &tree_oid);

            const std::string committer_name_used = committer_name.empty() ? author_name : committer_name;
            const std::string committer_email_used = committer_email.empty() ? author_email : committer_email;

            GitSignatureObj author_sig;
            author_sig.Now(author_name, author_email);

            GitSignatureObj committer_sig;
            committer_sig.Now(committer_name_used, committer_email_used);

            GitCommit head = HeadCommit();

            git_oid commit_oid;
            const git_commit* parents[] = { head.GetRaw() };

            GitException::Check(git_commit_create(&commit_oid, repo, "HEAD",
                author_sig.GetRaw(), committer_sig.GetRaw(),
                nullptr, message.c_str(),
                tree.GetRaw(), 1, parents));

            char oid_str[GIT_OID_HEXSZ + 1];
            git_oid_fmt(oid_str, &commit_oid);
            oid_str[GIT_OID_HEXSZ] = '\0';
            return oid_str;
        }

        std::string GetName() const {
            if (!ref) return "";
            const char* name = nullptr;
            git_branch_name(&name, ref);
            return name ? name : "";
        }

        std::string GetFullName() const {
            if (!ref) return "";
            return git_reference_name(ref);
        }

        GitCommit HeadCommit() const {
            if (!ref) throw GitException("Branch not loaded");

            const git_oid* oid = git_reference_target(ref);
            if (!oid) throw GitException("Branch has no target");

            return GitCommit(repo, oid);
        }

        bool IsHead() const {
            if (!ref) return false;
            return git_branch_is_head(ref) == 1;
        }

        git_branch_t GetType() const {
            return type;
        }

        git_reference* GetRaw() const {
            return ref;
        }

        bool IsValid() const {
            return ref != nullptr;
        }

        void Clear() {
            if (ref && owned) {
                git_reference_free(ref);
            }
            ref = nullptr;
            owned = false;
        }

        GitBranch(const GitBranch&) = delete;
        GitBranch& operator=(const GitBranch&) = delete;
        GitBranch(GitBranch&& other) noexcept
            : type(other.type), ref(other.ref), owned(other.owned) {
            other.ref = nullptr;
            other.owned = false;
        }

        GitBranch& operator=(GitBranch&& other) noexcept {
            if (this != &other) {
                Clear();
                type = other.type;
                ref = other.ref;
                owned = other.owned;
                other.ref = nullptr;
                other.owned = false;
            }
            return *this;
        }
    };

    class GitRepository {
    private:
        struct CallbackPayload {
            std::function<void(const std::string&)> progress;
            std::function<void(int, int, int)> transfer;
            GitCredentials credentials;
        };

        CallbackPayload cb_payload;

        git_repository* repo = nullptr;
        std::string directory;
        bool isOpen = false;

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

            // 1. HTTPS: Personal Access Token
            if (creds->type == GitCredentials::Type::TOKEN &&
                (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT))
            {
                if (creds->token.empty()) {
                    return GIT_EAUTH;
                }

                std::string username = creds->username.empty()
                    ? (username_from_url ? username_from_url : "x-access-token")
                    : creds->username;

                return git_cred_userpass_plaintext_new(out, username.c_str(), creds->token.c_str());
            }

            // 2. SSH: приватный ключ
            if (creds->type == GitCredentials::Type::SSH_KEY &&
                (allowed_types & GIT_CREDENTIAL_SSH_KEY))
            {
                if (creds->ssh_private_key_path.empty()) {
                    return GIT_EAUTH;
                }

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
                context->progress(std::string(str, len));
            }
            return 0;
        }

        static int TransferProgressCallback(const git_transfer_progress* stats, void* payload) {
            if (!payload || !stats) return 0;
            auto* context = static_cast<CallbackPayload*>(payload);
            if (context && context->transfer) {
                context->transfer(stats->received_objects, stats->total_objects, stats->indexed_objects);
            }
            return 0;
        }

        void InitialCallbacks(git_remote_callbacks& callbacks) {
            callbacks.payload = &cb_payload;

            callbacks.credentials = CredentialsCallback;
            callbacks.sideband_progress = ProgressCallback;
            callbacks.transfer_progress = TransferProgressCallback;

            callbacks.certificate_check = [](git_cert* /*cert*/, int valid, const char* /*host*/, void* /*payload*/) -> int {
                return valid ? 0 : 1;
                };
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
            std::function<void(const std::string&)> progress_callback = nullptr,
            std::function<void(int, int, int)> transfer_callback = nullptr)
        {
            cb_payload.progress = progress_callback;
            cb_payload.transfer = transfer_callback;

            git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
            InitialCallbacks(opts.fetch_opts.callbacks);

            Clone(path, url, opts);
        }

        void Close() {
            if (repo) {
                git_repository_free(repo);
                repo = nullptr;
                isOpen = false;
                directory.clear();
            }
        }

        GitStatusList GetStatus(unsigned int flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED) {
            if (!isOpen) throw GitException("Repository not open");
            return GitStatusList(repo, flags);
        }

        bool HasChanges() {
            auto status = GetStatus(GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                GIT_STATUS_OPT_INCLUDE_IGNORED);
            return status.IsModified();
        }

        GitCommit GetHeadCommit() {
            if (!isOpen) throw GitException("Repository not open");
            GitBranch branch = GetHeadBranch();
            return branch.HeadCommit();
        }

        GitBranch GetHeadBranch() {
            if (!isOpen) throw GitException("Repository not open");
            GitBranch branch;
            branch.Load(repo, GIT_BRANCH_LOCAL);
            return branch;
        }

        GitBranch GetBranch(const std::string& name, git_branch_t type = GIT_BRANCH_LOCAL) {
            if (!isOpen) throw GitException("Repository not open");
            GitBranch branch;
            branch.Load(repo, name, type);
            return branch;
        }

        std::vector<GitBranch> GetBranches(git_branch_t type = GIT_BRANCH_ALL) {
            if (!isOpen) throw GitException("Repository not open");

            std::vector<GitBranch> branches;

            git_branch_iterator* iter = nullptr;
            GitException::Check(git_branch_iterator_new(&iter, repo, type));

            git_reference* ref = nullptr;
            git_branch_t branch_type;

            while (git_branch_next(&ref, &branch_type, iter) == 0) {
                try {
                    GitBranch branch;
                    branch.Load(repo, ref, branch_type);
                    branches.push_back(std::move(branch));
                }
                catch (const GitException&) {
                    if (ref) {
                        git_reference_free(ref);
                        ref = nullptr;
                    }
                }
            }

            if (ref) {
                git_reference_free(ref);
            }

            git_branch_iterator_free(iter);
            return branches;
        }

        std::string CreateCommit(const std::string& message,
            const std::string& author_name,
            const std::string& author_email,
            const std::string& committer_name = "",
            const std::string& committer_email = "") {
            if (!isOpen) throw GitException("Repository not open");
            return GetHeadBranch().CreateCommit(message, author_name, author_email,
                committer_name, committer_email);
        }

        std::vector<std::string> GetRemotes() const {
            if (!isOpen) throw GitException("Repository not open");

            std::vector<std::string> remotes;
            git_strarray remote_names;
            GitException::Check(git_remote_list(&remote_names, repo));

            for (size_t i = 0; i < remote_names.count; ++i) {
                remotes.push_back(remote_names.strings[i]);
            }

            git_strarray_dispose(&remote_names);
            return remotes;
        }

        void AddRemote(const std::string& name, const std::string& url) {
            if (!isOpen) throw GitException("Repository not open");

            git_remote* remote = nullptr;
            GitException::Check(git_remote_create(&remote, repo, name.c_str(), url.c_str()));
            git_remote_free(remote);
        }

        void RemoveRemote(const std::string& name) {
            if (!isOpen) throw GitException("Repository not open");
            GitException::Check(git_remote_delete(repo, name.c_str()));
        }

        void Fetch(const std::string& remote_name = "origin",
            std::function<void(const std::string&)> progress_callback = nullptr,
            std::function<void(int, int, int)> transfer_callback = nullptr)
        {
            if (!isOpen) throw GitException("Repository not open");

            git_remote* remote = nullptr;
            GitException::Check(git_remote_lookup(&remote, repo, remote_name.c_str()));

            cb_payload.progress = progress_callback;
            cb_payload.transfer = transfer_callback;
            cb_payload.credentials.attempt_count = 0;

            git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks);

            int error = git_remote_fetch(remote, nullptr, &opts, nullptr);
            git_remote_free(remote);
            GitException::Check(error);
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
        }

        void Pull(const std::string& remote_name = "origin",
            std::function<void(const std::string&)> progress_callback = nullptr,
            std::function<void(int, int, int)> transfer_callback = nullptr)
        {
            try {
                std::cout << "Fetching from " << remote_name << "..." << std::endl;
                Fetch(remote_name, progress_callback, transfer_callback);

                std::cout << "Merging changes..." << std::endl;
                MergeFastForward(remote_name);

                std::cout << "Pull completed successfully" << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "Pull failed: " << e.what() << std::endl;
                throw;
            }
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

        void Push(const std::string& remote_name = "origin", const std::string& branch = "",
            std::function<void(const std::string&)> progress_callback = nullptr,
            std::function<void(int, int, int)> transfer_callback = nullptr)
        {
            if (!isOpen) throw GitException("Repository not open");

            git_remote* remote = nullptr;
            GitException::Check(git_remote_lookup(&remote, repo, remote_name.c_str()));

            std::string branch_name = branch.empty() ? GetHeadBranch().GetName() : branch;
            if (branch_name.empty()) {
                git_remote_free(remote);
                throw GitException("No branch specified and no current branch");
            }

            std::string branch_ref = "refs/heads/" + branch_name;

            cb_payload.progress = progress_callback;
            cb_payload.transfer = transfer_callback;
            cb_payload.credentials.attempt_count = 0;

            git_push_options opts = GIT_PUSH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks);

            const char* refspec = branch_ref.c_str();
            git_strarray refspecs;
            refspecs.count = 1;
            refspecs.strings = (char**)&refspec;

            int error = git_remote_push(remote, &refspecs, &opts);
            git_remote_free(remote);
            GitException::Check(error);
        }

        std::string GetPath() const {
            return directory;
        }

        bool IsOpen() const {
            return isOpen;
        }

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

        git_repository* GetRaw() const {
            return repo;
        }

        bool operator==(const GitRepository& other) const {
            return repo == other.repo;
        }

        bool operator!=(const GitRepository& other) const {
            return !(*this == other);
        }

        GitRepository(const GitRepository&) = delete;
        GitRepository& operator=(const GitRepository&) = delete;

        GitRepository(GitRepository&& other) noexcept
            : cb_payload(std::move(other.cb_payload)),
            repo(other.repo),
            directory(std::move(other.directory)),
            isOpen(other.isOpen)
        {
            other.repo = nullptr;
            other.isOpen = false;
        }

        GitRepository& operator=(GitRepository&& other) noexcept {
            if (this != &other) {
                Close();
                cb_payload = std::move(other.cb_payload);
                repo = other.repo;
                directory = std::move(other.directory);
                isOpen = other.isOpen;
                other.repo = nullptr;
                other.isOpen = false;
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
        Git() {
            Initialize();
        }

        ~Git() {
            if (isInitialized) {
                git_libgit2_shutdown();
                isInitialized = false;
            }
        }

        GitVersion GetVersion() const {
            return version;
        }

        std::string GetVersionString() const {
            return version.ToString();
        }

        int GetFeatures() const {
            return git_libgit2_features();
        }

        bool HasFeature(int feature) const {
            return (GetFeatures() & feature) != 0;
        }

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
            std::function<void(const std::string&)> progress = nullptr,
            std::function<void(int, int, int)> transfer = nullptr)
        {
            GitRepository repo;
            repo.SetCredentials(creds);
            repo.Clone(url, path, progress, transfer);
            return repo;
        }

        GitRepository CloneRepository(const std::string& url,
            const std::string& path,
            std::function<void(const std::string&)> progress = nullptr,
            std::function<void(int, int, int)> transfer = nullptr)
        {
            return CloneRepository(url, path, GitCredentials::Anonymous(), progress, transfer);
        }
    };

} // namespace GitWrapper

inline void Progress(const std::string& progress) {
    std::cout << "[PROGRESS] " << progress << std::flush;
}

inline void Transfer(int received_objects, int total_objects, int indexed_objects) {
    static int last_percent = -1;

    if (total_objects > 0) {
        int percent = (received_objects * 100) / total_objects;

        if (percent != last_percent) {
            last_percent = percent;

            std::cout << "\rReceiving objects: " << percent << "% ("
                << received_objects << "/" << total_objects << "), "
                << indexed_objects << " indexed" << std::flush;
        }
    }
}

int main() {
    
    setlocale(LC_ALL, "Russian");

    try {
        GitWrapper::Git git;

        auto token_creds = GitWrapper::GitCredentials::Token(USER_NAME, USER_GHP_TOKEN);

        /*
        auto repository_test = git.CloneRepository(
            USER_URL_REPO,
            "./repo_token",
            token_creds,
            &Progress,
            &Transfer
        );
        */

        auto repository_test = git.OpenRepository(
            "./repo_token", token_creds
        );

        std::cout << "Path: " << repository_test.GetPath() << std::endl;

        auto main_branch = repository_test.GetBranch("main");

        std::cout << "branch name: [" << main_branch.GetName() << "]\n";
        std::cout << "branch is head ? " << (main_branch.IsHead() ? "YES" : "NO") << "\n";

        auto head_commit = main_branch.HeadCommit();
        auto head_info = head_commit.GetInfo();

        std::cout << "Author: " << head_info.author.name
            << " <" << head_info.author.email << ">\n";
        std::cout << "Message: " << Utf8ToWindows1251(head_info.message) << "\n";

        std::string hash_commit = main_branch.CreateCommit(
            "Коммит #" + make_random_name(),
            head_info.author.name,
            head_info.author.email);
        std::cout << "Hash commit: " << hash_commit << "\n";

        // Push с прогрессом
        std::cout << "Pushing to origin..." << std::endl;
        repository_test.Push("origin", "", Progress, Transfer);
        std::cout << "\nCommit pushed!" << "\n";

        // Pull тоже с прогрессом
        repository_test.Pull("origin", Progress, Transfer);
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

