.. qtng documentation master file, created by
   sphinx-quickstart on Fri Nov 10 11:50:39 2017.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

Welcome to qtng's documentation!
=======================================

Source Code
-----------

https://github.com/hgoldfish/qtng/

Language
--------

Choose a language for document reading

English:    https://qtng.org/index.html

中文:       https://qtng.org/index.HANS.html


Author
------

Qize Huang <hgoldfish#gmail.com>

Feel free to send feedback to me.

A glance at qtng
------------------------

qtng is a coroutine-based network toolkit, like boost::asio but inspired by Python gevent. **Version 2.0** requires C++11 (C++17 and a system-installed Catch2 v3 to build the bundled tests). Here comes a simple example to get web pages.

.. code-block:: c++
    
    #include "qtng.h"
    
    int main()
    {
        qtng::HttpSession session;
        qtng::HttpResponse r = session.get("http://www.example.com/");
        if (r.isOk()) {
            ngDebug() << r.html();
        } else {
            ngDebug() << "failed.";
        }
        return 0;
    }
    
And another example to make IPv4 tcp connection.

.. code-block:: c++
    
    #include "qtng.h"
    
    int main()
    {
        qtng::Socket conn;
        conn.connect("news.163.com", 80);
        conn.sendall("GET / HTTP/1.0\r\n\r\n");
        ngDebug() << conn.recv(1024 * 8);
        return 0;
    }

To create IPv4 tcp server.

.. code-block:: c++
    
    Socket s;
    CoroutineGroup workers;
    s.bind(HostAddress::AnyIPv4, 8000);
    s.listen(100);
    while (true) {
        std::shared_ptr<Socket> request(s.accept());
        if (!request) {
            break;
        }
        workers.spawn([request] {
            request->sendall("hello!");
            request->close();
        });
    }

As you can see, networking programming is done with very straightforward API.

Give it a try (for linux). ::

    git clone https://github.com/hgoldfish/qtng.git
    cd qtng
    mkdir build && cd build
    cmake ..
    cmake --build .
    sudo cmake --install . --prefix /usr/local   # optional

Installed layout: umbrella header at ``include/qtng.h``, module headers under ``include/qtng/``, static library as ``libqtng.a`` (``qtng.lib`` on MSVC). Use ``#include <qtng.h>`` or ``#include <qtng/coroutine.h>`` (or ``#include <qtng/qtng.h>``) after installation.


User Guide
==========

.. toctree::
   :maxdepth: 3

   intro
   practices
   references
   index.HANS
   
Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`