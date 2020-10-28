#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <string>
#include <queue>
#include <chrono>
#include <atomic>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <boost/regex.hpp>
#include <boost/filesystem.hpp>

std::vector<std::string>** url_buffer_a;
std::vector<std::string>** content_buffer_a;
std::atomic<int>* switch_url_buffer_a;
std::atomic<int>* switch_content_buffer_a;
std::atomic<bool> stop;

int* total_time_a;
int* eff_time_a;

size_t string_write(void *contents, size_t size, size_t nmemb, void *userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// set CURLOPT_NOSIGNAL to prevent segfault
// http://stackoverflow.com/questions/9191668/error-longjmp-causes-uninitialized-stack-frame
CURLcode curl_read(const std::string& url, std::string& buffer, long timeout = 30)
{
    CURLcode code(CURLE_FAILED_INIT);
    CURL* curl = curl_easy_init();

    if (curl) {
        if (CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_URL, url.c_str()))
                && CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write))
                && CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer))
                && CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L))
                && CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L))
                && CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout))
                && CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1)))
        {
            code = curl_easy_perform(curl);
        }
        curl_easy_cleanup(curl);
    }
    return code;
}

void fetch(const int id)
{
    int n_url_buf = 0;
    unsigned i_url = 0;
    int n_content_buf = 0;

    std::chrono::high_resolution_clock::time_point start, end;
    start = std::chrono::high_resolution_clock::now();
    int effective_time = 0;

    while (!stop)
    {

        auto& url_buffer = url_buffer_a[id][n_url_buf];
        auto& content_buffer = content_buffer_a[id][n_content_buf];
        auto& switch_url_buffer = switch_url_buffer_a[id];
        auto& switch_content_buffer = switch_content_buffer_a[id];

        std::string depth;
        std::string url;

        bool not_empty = false;
        if (i_url < url_buffer.size())
        {
            not_empty = true;
            depth = url_buffer[i_url++];
            url = url_buffer[i_url++];
        }

        if (not_empty)
        {
            std::chrono::high_resolution_clock::time_point start, end;
            start = std::chrono::high_resolution_clock::now();

//		 	std::cout << "Fetching " << url << std::endl;

            std::string content;
            CURLcode res = curl_read(url, content);

            if (res == CURLE_OK)
            {
                content_buffer.push_back(depth);
                content_buffer.push_back(url);
                content_buffer.push_back(content);
            }
            else if (res == CURLE_COULDNT_RESOLVE_HOST);
            else if (res == CURLE_URL_MALFORMAT);
            else if (res == CURLE_OPERATION_TIMEDOUT);
            else
                throw std::runtime_error(std::string("CURL: ") + curl_easy_strerror(res));

            end = std::chrono::high_resolution_clock::now();
            effective_time += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        }

        if (switch_url_buffer == 0 && i_url == url_buffer.size()) {
            switch_url_buffer = 1;
        }

        if (switch_url_buffer == 2)
        {
            switch_url_buffer = 0;
            n_url_buf = (n_url_buf + 1) % 2;
            i_url = 0;
        }

        if (switch_content_buffer == 1 && !content_buffer.empty())
        {
            switch_content_buffer = 2;
            n_content_buf = (n_content_buf + 1) % 2;
            content_buffer_a[id][n_content_buf].clear();
        }

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    end = std::chrono::high_resolution_clock::now();
    total_time_a[id] = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    eff_time_a[id] = effective_time;
}

void extract(int max_depth, int max_count, const std::string& storage_path, int thread_num)
{

    int n_url_buf_a[thread_num];
    int n_content_buf_a[thread_num];
    unsigned i_content_a[thread_num];

    std::unordered_map<std::string, bool> visited;

    for(int i = 0; i < thread_num; i++) n_url_buf_a[i] = 1;
    for(int i = 0; i < thread_num; i++) n_content_buf_a[i] = 1;
    for(int i = 0; i < thread_num; i++) i_content_a[i] = 0;

    int tid = -1;
    int url_id = -1;

    std::chrono::high_resolution_clock::time_point start, end;
    start = std::chrono::high_resolution_clock::now();
    int loss_time = 0;
    int max_loss_time = 0;

    int waiting_fetchers = 0;

    while (true)
    {
        tid = (tid + 1) % thread_num;

        if (tid == 0)
        {
            if (waiting_fetchers == thread_num)
            {
                bool stop1 = true;
                // we should stop if all fetchers are waiting for new links and extractor
                // didn't extract new links and won't extract ones
                // extractor won't extract new links if it is waiting for new content and
                // fetchers aren't downloading pages
                // there is guarantee that if fetchers are waiting for new links than
                // they aren't downloading anything
                for(int i = 0; i < thread_num; i++)
                    if (!(switch_url_buffer_a[i] == 1 && url_buffer_a[i][n_url_buf_a[i]].empty()
                            && switch_content_buffer_a[i] == 1))
                    {
                        stop1 = false;
                        break;
                    }
                if (stop1)
                {
                    stop = true;
                    break;
                }
            }
            waiting_fetchers = 0;
        }

        int& n_content_buf = n_content_buf_a[tid];
        auto& content_buffer = content_buffer_a[tid][n_content_buf];

        unsigned& i_content = i_content_a[tid];

        auto& switch_url_buffer = switch_url_buffer_a[tid];
        auto& switch_content_buffer = switch_content_buffer_a[tid];

        std::string depth;
        int depth_i;
        std::string url;
        std::string content;

        bool not_empty = false;
        if (i_content < content_buffer.size())
        {
            not_empty = true;
            depth = content_buffer[i_content++];
            depth_i = stoi(depth);
            url = content_buffer[i_content++];
            content = content_buffer[i_content++];
        }

        if (switch_content_buffer == 0 && i_content == content_buffer.size()) {
            switch_content_buffer = 1;
        }

        if (not_empty && depth_i < max_depth)
        {
            std::chrono::high_resolution_clock::time_point start, end;
            start = std::chrono::high_resolution_clock::now();

            std::string domain;
            std::size_t pos = url.find("//");
            pos = url.find("/", pos + strlen("//"));
            domain = url.substr(0, pos);

            boost::regex r("<a\\s+href=\"?([\\-:\\w\\d\\.\\/]+)\"?");
            boost::smatch m;
            std::string::const_iterator s = content.begin(), e = content.end();

            depth = std::to_string(depth_i + 1);

            while (boost::regex_search(s, e, m, r))
            {
                url_id = (url_id + 1) % thread_num;

                int& n_url_buf = n_url_buf_a[url_id];
                auto& url_buffer = url_buffer_a[url_id][n_url_buf];

                std::string link = m[1];
                std::size_t pos = link.find("//");
                if (pos == std::string::npos)
                {
                    if (link.front() != '/')
                    {
                        if (visited.find(link) == visited.end())
                        {
                            visited.insert({link, true});
                            url_buffer.push_back(depth);
                            url_buffer.push_back(link);
                        }
                    }
                    std::string abs_link = (boost::filesystem::path(domain) / boost::filesystem::path(link)).string();
                    if (visited.find(abs_link) == visited.end())
                    {
                        visited.insert({abs_link, true});
                        url_buffer.push_back(depth);
                        url_buffer.push_back(abs_link);
                    }
                } else {
                    if (visited.find(link) == visited.end())
                    {
                        visited.insert({link, true});
                        url_buffer.push_back(depth);
                        url_buffer.push_back(link);
                    }
                }

                s = m[0].second;
            }

            {
                std::size_t pos = url.find("//");
                pos = (pos == std::string::npos) ? 0 : pos + strlen("//");
                std::string path = (boost::filesystem::path(storage_path) 
					/ boost::filesystem::path(url.substr(pos))).string() + "_index.html";
                std::string dir = path.substr(0, path.find_last_of("/"));
                boost::filesystem::create_directories(dir);
                std::ofstream page_file(path);
                if (page_file.fail()) throw std::runtime_error(strerror(errno));
                page_file << content;
            }



            end = std::chrono::high_resolution_clock::now();
            int diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            loss_time += diff;
            max_loss_time = std::max(max_loss_time, diff);

            if (!max_count--)
            {
                stop = true;
                break;
            }
        }

        if (switch_content_buffer == 2)
        {
            switch_content_buffer = 0;
            n_content_buf = (n_content_buf + 1) % 2;
            i_content = 0;
        }

        int& n_url_buf = n_url_buf_a[tid];
        if (switch_url_buffer == 1 && !url_buffer_a[tid][n_url_buf].empty())
        {
            switch_url_buffer = 2;
            n_url_buf = (n_url_buf + 1) % 2;
            url_buffer_a[tid][n_url_buf].clear();

        }

        if (switch_url_buffer == 1)
            waiting_fetchers++;
			
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }


    end = std::chrono::high_resolution_clock::now();
    int total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Extractor total_time=" << total_time << " effective_time=" << total_time - loss_time
              << " max_delay=" << max_loss_time << std::endl;

}

int main(int argc, const char* argv[])
{
    std::string start_url = (argc > 1) ? argv[1] : "www.ya.ru";
    int max_depth = (argc > 2) ? atoi(argv[2]) : 2;
    int max_count = (argc > 3) ? atoi(argv[3]) : 42;
    std::string storage_path = (argc > 4) ? argv[4] : "/home/vlad/crawler/pages/";
    int thread_num = (argc > 5) ? atoi(argv[5]) : 1;

    if (thread_num < 1) thread_num = 1;

    // allocation
    url_buffer_a = new std::vector<std::string>*[thread_num];
    for(int i = 0; i < thread_num; i++)
        url_buffer_a[i] = new std::vector<std::string>[2];

    content_buffer_a = new std::vector<std::string>*[thread_num];
    for(int i = 0; i < thread_num; i++)
        content_buffer_a[i] = new std::vector<std::string>[2];

    switch_url_buffer_a = new std::atomic<int>[thread_num];
    switch_content_buffer_a = new std::atomic<int>[thread_num];

    total_time_a = new int[thread_num];
    eff_time_a = new int[thread_num];


    // push first url to tid0
    url_buffer_a[0][0].push_back("0"); // depth
    url_buffer_a[0][0].push_back(start_url);


    //stop = false;
    std::vector<std::thread> fetchers;

    std::chrono::high_resolution_clock::time_point start, end, end1;
    start = std::chrono::high_resolution_clock::now();

    std::thread extractor(extract, max_depth, max_count, storage_path, thread_num);
    for(int i = 0; i < thread_num; i++)
        fetchers.emplace_back(fetch, i);
    extractor.join();
    stop = true;
    for(auto &f : fetchers)
        f.join();

    end = std::chrono::high_resolution_clock::now();
    int total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();


    int loss_sum = 0;
    for(int i = 0; i < thread_num; i++)
    {
        int tt = total_time_a[i];
        int et = eff_time_a[i];
        loss_sum += tt - et;

        std::cout << "Fetcher(" << i << ") total_time=" << tt
                  << " effective_time=" << et
                  << " loss " << tt - et
                  << " [" << 100.0 * (tt - et) / tt << "%]" << std::endl;
    }

    std::cout << "Total time: " << total_time << std::endl;
    std::cout << "Average loss: " << loss_sum / thread_num  << std::endl;

    delete [] total_time_a;
    delete [] eff_time_a;

    delete [] switch_url_buffer_a;
    delete [] switch_content_buffer_a;

    for(int i = 0; i < thread_num; i++) delete [] url_buffer_a[i];
    delete [] url_buffer_a;

    for(int i = 0; i < thread_num; i++) delete [] content_buffer_a[i];
    delete [] content_buffer_a;

    return 0;
}
