#/ob/oceanbase-master

build="build_"
loc1="${build}$1"
loc2="./${loc1}/src/observer/observer"
loc3="/root/.obd/repository/oceanbase-ce/4.3.0.1/oceanbase-ce/bin/"
echo $loc1
echo $loc2
echo $loc3

#build_debug or build_release ob
bash build.sh $1 -DOB_USE_CCACHE=ON --init --make -j16
#cd
cd ../
#stop obcluster
obd cluster stop obcluster
#wait 
sleep 3s
#cp replacement
cp $loc2 $loc3
