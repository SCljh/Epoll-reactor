# Epoll reactor模式实现
核心结构体：ntyevent，将每个fd与callback函数绑定，并注册监听事件。
