NUK_URLLC
========

Base on srsLTE 19.09 version(https://github.com/srsLTE/srsLTE/tree/release_19_09)

Hardware
--------

* ASUS M580V
* USRP B210

Build Instructions
------------------
* on Ubuntu 16.04



```
sudo apt-get install cmake libfftw3-dev libmbedtls-dev libboost-program-options-dev libconfig++-dev libsctp-dev
```


* RF front-end driver:
  * UHD:                 https://github.com/EttusResearch/uhd

Download and build srsLTE: 
```
git clone https://github.com/Nukicslab/nukxURLLC.git
cd srsLTE
mkdir build
cd build
cmake ../
make
make test
```

Install srsLTE:

```
sudo make install
srslte_install_configs.sh user
```

This installs srsLTE and also copies the default srsLTE config files to
the user's home directory (~/.config/srslte).



