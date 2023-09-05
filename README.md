# "Parallel File Find" - C Implementation using Threads

The goal of this assignment was for me to gain experience with threads and filesystem system calls. In this assignment, I created a program that searches a directory tree for files whose name matches some search term. I received a directory 'D' and a search term 'T' as inputs and found every file in 'D’s' directory tree whose name contains 'T.' To parallelize my work, I used threads. Specifically, I tasked different threads with searching individual directories.
