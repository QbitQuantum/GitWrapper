#pragma once
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
        time_t time;
        int offset;  // часовой пояс в минутах

        std::string ToString() const {
            return name + " <" + email + ">";
        }
    };

    struct GitCommitInfo {
        std::string id;
        std::string message;
        GitSignature author;
        GitSignature committer;
        time_t commit_time;
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

        // Для анонимного доступа
        static GitCredentials Anonymous() {
            GitCredentials creds;
            creds.type = Type::NONE;
            return creds;
        }

        // Для доступа по SSH ключу
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

        /*
        // Для доступа через SSH агент
        static GitCredentials SSH_AGENT(const std::string& user = "") {
            GitCredentials creds;
            creds.type = Type::SSH_AGENT;
            creds.username = user;
            return creds;
        }
        */

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

        // Запрещаем копирование
        GitStatusList(const GitStatusList&) = delete;
        GitStatusList& operator=(const GitStatusList&) = delete;

        // Разрешаем перемещение
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

        void Load(git_repository* repo, const std::string& oid_str) {
            git_oid oid;
            int error = git_oid_fromstr(&oid, oid_str.c_str());
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

        // Запрещаем копирование
        GitCommit(const GitCommit&) = delete;
        GitCommit& operator=(const GitCommit&) = delete;

        // Разрешаем перемещение
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

    class GitBranch {
    private:
        git_branch_t type;
        git_reference* ref = nullptr;
        bool owned = false;

    public:
        GitBranch() : type(GIT_BRANCH_LOCAL) {}

        ~GitBranch() {
            Clear();
        }

        void Load(git_repository* repo, const std::string& name, git_branch_t branch_type = GIT_BRANCH_LOCAL) {
            Clear();
            int error = git_branch_lookup(&ref, repo, name.c_str(), branch_type);
            GitException::Check(error);
            owned = true;
            type = branch_type;
        }

        void Load(git_reference* ref, git_branch_t branch_type) {
            Clear();
            this->ref = ref;
            owned = false;
            this->type = branch_type;
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

        GitCommit GetCommit(git_repository* repo) const {
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
        
        // Добавляем callback для индикации прогресса
        struct CallbackPayload {
            std::function<void(const std::string&)>* progress = nullptr;
            std::function<void(int, int, int)>* transfer = nullptr;
            GitCredentials credentials;
        };

        CallbackPayload cb_payload = CallbackPayload();
        
        git_repository* repo = nullptr;
        std::string directory;
        bool isOpen = false;

        static int CredentialsCallback(
            git_cred** out,
            const char* url,
            const char* username_from_url,
            unsigned int allowed_types, void* payload)
        {
            // Если payload пустой, мы не можем получить токен или ключ — прерываем операцию
            if (!payload) {
                return GIT_EAUTH;
            }

            CallbackPayload* context = (CallbackPayload*)payload;
            
            // Если context неккоректный, мы не можем получить токен или ключ — прерываем операцию
            if (!context) {
                return GIT_EAUTH;
            }

            auto creds = &context->credentials;

            // Защита от бесконечного цикла при неверном токене или ключе
            if (creds->attempt_count > 0) {
                return GIT_EAUTH;
            }
            creds->attempt_count++;

            // 1. Современный HTTPS: Авторизация через Personal Access Token
            if (creds->type == GitCredentials::Type::TOKEN && (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT)) {
                if (creds->token.empty()) {
                    return GIT_EAUTH; // Без токена выполнение современного HTTPS невозможно
                }

                // Для GitHub/GitLab имя пользователя может быть любым (например, "git"), 
                // но если оно есть в URL или структуре — используем его.
                std::string username = creds->username.empty() ?
                    (username_from_url ? username_from_url : "git") :
                    creds->username;

                // Токен передается в целевое поле пароля
                return git_cred_userpass_plaintext_new(out, username.c_str(), creds->token.c_str());
            }

            // 2. Современный SSH: Авторизация по приватным ключам
            if (creds->type == GitCredentials::Type::SSH_KEY && (allowed_types & GIT_CREDENTIAL_SSH_KEY)) {
                if (creds->ssh_private_key_path.empty()) {
                    return GIT_EAUTH; // Без приватного ключа SSH-авторизация невозможна
                }

                return git_cred_ssh_key_new(out,
                    creds->username.empty() ? "git" : creds->username.c_str(),
                    creds->ssh_public_key_path.empty() ? nullptr : creds->ssh_public_key_path.c_str(),
                    creds->ssh_private_key_path.c_str(),
                    creds->ssh_passphrase.empty() ? nullptr : creds->ssh_passphrase.c_str());
            }

            // Если тип credentials не совпадает с тем, что просит сервер
            return GIT_PASSTHROUGH;
        }


        static int ProgressCallback(const char* str, int len, void* payload) {
            if (!payload || !(str && len > 0)) return 0;
            if (CallbackPayload* context = (CallbackPayload*)payload; context && context->progress)
                (*context->progress)(std::string(str, len));
            return 0;
        }

        static int TransferProgressCallback(const git_transfer_progress* stats, void* payload) {
            if (!payload || !stats) return 0;
            if (CallbackPayload* context = (CallbackPayload*)payload; context && context->transfer)
                (*context->transfer)(stats->received_objects, stats->total_objects, stats->indexed_objects);
            return 0;
        }

        void InitialCallbacks(
            git_remote_callbacks& callbacks, 
            std::function<void(const std::string&)> progress_callback = nullptr,
            std::function<void(int, int, int)> transfer_callback = nullptr) {
            
            cb_payload.progress = &progress_callback;
            cb_payload.transfer = &transfer_callback;

            // Лишний раз в клюбэках не проверять на наличие указателей функций
            if (cb_payload.progress && cb_payload.transfer)
                callbacks.payload = &cb_payload;

            callbacks.credentials = CredentialsCallback;
            callbacks.sideband_progress = ProgressCallback;
            callbacks.transfer_progress = TransferProgressCallback;

            callbacks.certificate_check = [](git_cert* cert, int valid, const char* host, void* payload) -> int {
                (void)cert;
                (void)host;
                (void)payload;
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

        // Создать новый репозиторий
        void Init(const std::string& path, bool bare = false) {
            Close();
            directory = path;
            int error = git_repository_init(&repo, directory.c_str(), bare);
            GitException::Check(error);
            isOpen = true;
        }

        // Открыть существующий репозиторий
        void Open(const std::string& path) {
            Close();
            directory = path;
            int error = git_repository_open(&repo, path.c_str());
            GitException::Check(error);
            isOpen = true;
        }

        // Клонировать веб-репозиторий
        void Clone(const std::string& path, const std::string& url, const git_clone_options& opts) {
            Close();
            directory = path;
            int error = git_clone(&repo, url.c_str(), directory.c_str(), &opts);
            GitException::Check(error);
            isOpen = true;
        }

        // Установка учетных данных для аутентификации
        void SetCredentials(const GitCredentials& creds) {
            cb_payload.credentials = creds;
        }

        // Клонировать репозиторий
        void Clone(const std::string& url, const std::string& path,
            std::function<void(const std::string&)> progress_callback = nullptr,
            std::function<void(int, int, int)> transfer_callback = nullptr) {
            
            git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
            InitialCallbacks(opts.fetch_opts.callbacks, progress_callback, transfer_callback);

            Clone(path, url, opts);
        }

        void Close() {
            if (repo) {
                git_repository_free(repo);
                repo = nullptr;
                isOpen = false;
                directory = "";
            }
        }

        // Статус
        GitStatusList GetStatus(unsigned int flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED) {
            if (!isOpen) throw GitException("Repository not open");
            return GitStatusList(repo, flags);
        }

        bool HasChanges() {
            auto status = GetStatus(GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                GIT_STATUS_OPT_INCLUDE_IGNORED);
            return status.IsModified();
        }

        // Коммиты
        GitCommit GetHeadCommit() {
            if (!isOpen) throw GitException("Repository not open");

            git_reference* head = nullptr;
            int error = git_repository_head(&head, repo);
            GitException::Check(error);

            const git_oid* oid = git_reference_target(head);
            GitCommit commit(repo, oid);
            git_reference_free(head);

            return commit;
        }

        GitCommit GetCommit(const std::string& oid_str) {
            if (!isOpen) throw GitException("Repository not open");
            return GitCommit(repo, oid_str);
        }

        // Ветки
        GitBranch GetCurrentBranch() {
            if (!isOpen) throw GitException("Repository not open");

            git_reference* head = nullptr;
            int error = git_repository_head(&head, repo);
            GitException::Check(error);

            GitBranch branch;
            branch.Load(head, GIT_BRANCH_LOCAL);
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
            int error = git_branch_iterator_new(&iter, repo, type);
            GitException::Check(error);

            git_reference* ref = nullptr;
            git_branch_t branch_type;

            while (git_branch_next(&ref, &branch_type, iter) == 0) {
                try {
                    GitBranch branch;
                    branch.Load(ref, branch_type);
                    branches.push_back(std::move(branch));
                }
                catch (const GitException&) {
                    // Пропускаем ошибочные ветки
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

        // Создать коммит
        std::string CreateCommit(const std::string& message,
            const std::string& author_name,
            const std::string& author_email,
            const std::string& committer_name = "",
            const std::string& committer_email = "") {
            if (!isOpen) throw GitException("Repository not open");

            // Получаем индекс
            git_index* index = nullptr;
            int error = git_repository_index(&index, repo);
            GitException::Check(error);

            // Добавляем все изменения
            error = git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
            GitException::Check(error);

            // Пишем дерево
            git_oid tree_oid;
            error = git_index_write_tree(&tree_oid, index);
            GitException::Check(error);

            git_index_free(index);

            // Загружаем дерево
            git_tree* tree = nullptr;
            error = git_tree_lookup(&tree, repo, &tree_oid);
            GitException::Check(error);

            // Получаем HEAD
            git_reference* head = nullptr;
            error = git_repository_head(&head, repo);
            GitException::Check(error);

            // Загружаем родительский коммит
            const git_oid* parent_oid = git_reference_target(head);
            git_commit* parent = nullptr;
            error = git_commit_lookup(&parent, repo, parent_oid);
            GitException::Check(error);

            // Создаем подпись автора и коммитера
            const std::string committer_name_used = committer_name.empty() ? author_name : committer_name;
            const std::string committer_email_used = committer_email.empty() ? author_email : committer_email;

            git_signature* author_sig = nullptr;
            git_signature* committer_sig = nullptr;

            error = git_signature_new(&author_sig, author_name.c_str(), author_email.c_str(), time(nullptr), 0);
            GitException::Check(error);

            error = git_signature_new(&committer_sig, committer_name_used.c_str(),
                committer_email_used.c_str(), time(nullptr), 0);
            GitException::Check(error);

            // Создаем коммит
            git_oid commit_oid;
            const git_commit* parents[] = { parent };

            error = git_commit_create(&commit_oid, repo, "HEAD",
                author_sig, committer_sig,
                nullptr, message.c_str(),
                tree, 1, parents);

            git_signature_free(author_sig);
            git_signature_free(committer_sig);
            git_commit_free(parent);
            git_tree_free(tree);
            git_reference_free(head);

            GitException::Check(error);

            char oid_str[GIT_OID_HEXSZ + 1];
            git_oid_fmt(oid_str, &commit_oid);
            oid_str[GIT_OID_HEXSZ] = '\0';
            return oid_str;
        }

        // Работа с удаленными репозиториями
        std::vector<std::string> GetRemotes() const {
            if (!isOpen) throw GitException("Repository not open");

            std::vector<std::string> remotes;
            git_strarray remote_names;
            int error = git_remote_list(&remote_names, repo);
            GitException::Check(error);

            for (size_t i = 0; i < remote_names.count; ++i) {
                remotes.push_back(remote_names.strings[i]);
            }

            git_strarray_dispose(&remote_names);
            return remotes;
        }

        void AddRemote(const std::string& name, const std::string& url) {
            if (!isOpen) throw GitException("Repository not open");

            git_remote* remote = nullptr;
            int error = git_remote_create(&remote, repo, name.c_str(), url.c_str());
            GitException::Check(error);

            git_remote_free(remote);
        }

        void RemoveRemote(const std::string& name) {
            if (!isOpen) throw GitException("Repository not open");

            int error = git_remote_delete(repo, name.c_str());
            GitException::Check(error);
        }

        // Fetch
        void Fetch(const std::string& remote_name = "origin",
            std::function<void(const std::string&)> progress_callback = nullptr,
            std::function<void(int, int, int)> transfer_callback = nullptr) {
            if (!isOpen) throw GitException("Repository not open");

            git_remote* remote = nullptr;
            int error = git_remote_lookup(&remote, repo, remote_name.c_str());
            GitException::Check(error);

            git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks, progress_callback, transfer_callback);

            error = git_remote_fetch(remote, nullptr, &opts, nullptr);
            git_remote_free(remote);
            GitException::Check(error);
        }

        // Pull
        void Pull(const std::string& remote_name = "origin",
            std::function<void(const std::string&)> progress_callback = nullptr) {
            if (!isOpen) throw GitException("Repository not open");

            // Сначала fetch
            Fetch(remote_name, progress_callback);

            // Затем merge
            git_reference* head = nullptr;
            int error = git_repository_head(&head, repo);
            GitException::Check(error);

            // Получаем ветку из удаленного репозитория
            std::string branch_name = GetCurrentBranch().GetName();
            if (branch_name.empty()) {
                git_reference_free(head);
                throw GitException("No current branch");
            }

            std::string remote_branch_ref = "refs/remotes/" + remote_name + "/" + branch_name;

            git_annotated_commit* commit = nullptr;
            error = git_annotated_commit_from_fetchhead(&commit, repo,
                remote_branch_ref.c_str(), remote_name.c_str(),
                git_reference_target(head));

            if (error == 0) {
                git_merge_options merge_opts = GIT_MERGE_OPTIONS_INIT;
                git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
                checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE;

                error = git_merge(repo, (const git_annotated_commit**)&commit, 1, &merge_opts, &checkout_opts);

                if (error == 0) {
                    // Проверяем конфликты
                    git_index* index = nullptr;
                    error = git_repository_index(&index, repo);
                    if (error == 0) {
                        if (git_index_has_conflicts(index)) {
                            git_index_free(index);
                            git_annotated_commit_free(commit);
                            git_reference_free(head);
                            throw GitException("Merge conflicts detected");
                        }
                        git_index_free(index);
                    }
                }

                git_annotated_commit_free(commit);
            }

            git_reference_free(head);
            GitException::Check(error);
        }

        // Push
        void Push(const std::string& remote_name = "origin", const std::string& branch = "",
            std::function<void(const std::string&)> progress_callback = nullptr) {
            if (!isOpen) throw GitException("Repository not open");

            git_remote* remote = nullptr;
            int error = git_remote_lookup(&remote, repo, remote_name.c_str());
            GitException::Check(error);

            std::string branch_name = branch.empty() ? GetCurrentBranch().GetName() : branch;
            if (branch_name.empty()) {
                git_remote_free(remote);
                throw GitException("No branch specified and no current branch");
            }

            std::string branch_ref = "refs/heads/" + branch_name;

            git_push_options opts = GIT_PUSH_OPTIONS_INIT;
            InitialCallbacks(opts.callbacks, progress_callback);

            const char* refspec = branch_ref.c_str();
            git_strarray refspecs;
            refspecs.count = 1;
            refspecs.strings = (char**)&refspec;

            error = git_remote_push(remote, &refspecs, &opts);
            git_remote_free(remote);
            GitException::Check(error);
        }

        // Получить информацию о репозитории
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

        // Операторы сравнения
        bool operator==(const GitRepository& other) const {
            return repo == other.repo;
        }

        bool operator!=(const GitRepository& other) const {
            return !(*this == other);
        }

        // Запрещаем копирование
        GitRepository(const GitRepository&) = delete;
        GitRepository& operator=(const GitRepository&) = delete;

        // Разрешаем перемещение
        GitRepository(GitRepository&& other) noexcept
            : repo(other.repo), directory(std::move(other.directory)), isOpen(other.isOpen),
            cb_payload(std::move(other.cb_payload)) {
            other.repo = nullptr;
            other.isOpen = false;
        }

        GitRepository& operator=(GitRepository&& other) noexcept {
            if (this != &other) {
                Close();
                repo = other.repo;
                directory = std::move(other.directory);
                isOpen = other.isOpen;
                cb_payload = std::move(other.cb_payload);
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

        // Создание репозитория
        GitRepository OpenRepository(const std::string& path) {
            GitRepository repo;
            repo.Open(path);
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

void Progress(const std::string& progress) {
    std::cout << "[PROGRESS] " << progress << std::flush;
}

void Transfer(int received_objects, int total_objects, int indexed_objects) {
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
    try {
        GitWrapper::Git git;

        auto token_creds = GitWrapper::GitCredentials::Token(USER_NAME, USER_GHP_TOKEN);

        auto repo1 = git.CloneRepository(
            USER_URL_REPO,
            "./repo_token",
            token_creds,
            &Progress,
            &Transfer
        );

        std::cout << "Repository cloned successfully!" << std::endl;
        std::cout << "Path: " << repo1.GetPath() << std::endl;

        repo1.Close();

    }
    catch (const GitWrapper::GitException& e) {
        std::cerr << "Error: " << e.what() << std::endl;

        const git_error* err = git_error_last();
        if (err && err->message) {
            std::cerr << "Git error details: " << err->message << std::endl;
        }
    }

    return 0;
}