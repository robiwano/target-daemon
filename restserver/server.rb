require 'sinatra'
require 'socket'

s = TCPSocket.new 'localhost', 5000
s.setsockopt(Socket::SOL_SOCKET, Socket::SO_KEEPALIVE, false)

get '/frank-says' do
    'Put this in your pipe & smoke it!'
end 

put '/frank-says' do
    'Yiiihaaaa'
end
