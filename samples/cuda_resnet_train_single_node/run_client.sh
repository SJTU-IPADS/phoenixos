script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd $script_dir

if [ -f "./client_exist.txt" ]; then
    rm ./client_exist.txt
fi

LD_LIBRARY_PATH=../../remoting/submodules/libtirpc/install/lib:../../remoting/cpu/:../../lib LD_PRELOAD=../../remoting/cpu/cricket-client.so python3 ./train.py 


# nsys for client behaviour:
# nsys profile -t cuda,nvtx -o ./resnet_50_client_output --force-overwrite true python3 ./train.py 
# nsys profile -t cuda,nvtx -o ./resnet_50_client_output --export json --force-overwrite true python3 ./train.py 

# to download the nsys result
# scp -r hzb@meepo4:/disk1/hzb/projects/pos/reorg/phoenixos/samples/cuda_resnet_train_single_node/resnet_50_client_output.nsys-rep ~/Desktop
