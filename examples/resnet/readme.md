# PhoenixOS Sample: Resnet-152M Training / Inference

## Environment

This example is fully tested under:

* `pytorch=1.13.0a0+git2263262`
* `torchvision==0.15.2`
* CUDA 11.3

We have already built a docker image for running this example (`phoenixos/pytorch:11.3-ubuntu20.04`), you can pull and run the container by:

```bash
cd [REPO PATH]
docker run -dit --gpu all --privileged  --ipc=host --network=host \
            -v .:/root --name phos_example phoenixos/pytorch:11.3-ubuntu20.04

docker exec -it phos_example /bin/bash
```

## Run

After succesfully installed PhOS inside the container (See [Build and Install PhOS](https://github.com/SJTU-IPADS/PhoenixOS/tree/zhuobin/fix_cli?tab=readme-ov-file#i-build-and-install-phos)), you can run this example by:

1. Start PhOS daemon by simply runing:

    ```bash
    # inside container
    pos_cli --start daemon --detach
    ```

2. Running the training / inference script:

    ```bash
    # inside container
    cd /root/example/resnet

    # train
    env $phos python3 ./train.py

    # inference
    env $phos python3 ./inference.py
    ```