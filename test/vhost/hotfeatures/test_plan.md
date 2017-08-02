#Hotattach/hotdetach integration Test Plan

## Test Cases Description
1. During fio test use test/vhost/fiotest/fio_jobs/default_integrity.job
2. Test cases below contain manual steps for testing. Automated tests are located in test/vhost/hotfeatures.

### Hotattach Test Cases prerequisites
1. Get latest SPDK.
2. run "git submodule update --init" in SPDK directory.
3. Run vhost with 4 empty controllers. If don't have 4 disks use split.
4. Run vms, first with ctrlr-1 and ctrlr-2 and second one with ctrlr-3 and ctrlr-4

## Test Case 1
1. Attach NVMe to Ctrlr 1		
2. Check vhost or VMs did not crash		
3. Run fio integrity on attached device		
4. Reboot VMs		
5. Run fio integrity again		

## Test Case 2
1. Attach NVMe to Ctrlr 1			
2. Run fio integrity on attached device			
3. During fio attach another NVMe to Ctrlr 1			
4. Check vhost or VMs did not crash			
5. Run fio integrity on both devices			
6. Reboot VMs			
7. Run fio integrity again on all devices			

## Test Case 3
1. Attach NVMe to Ctrlr 1			
2. Run fio integrity on attached device			
3. During fio attach NVMe to Ctrl2			
4. Check vhost or VMs did not crash			
5. Run fio integrity on both devices			
6. Reboot VMs			
7. Run fio integrity again on all devices			

## Test Case 4
1. Attach NVMe to Ctrlr 1			
2. Run fio integrity on attached device			
3. During fio attach NVMe to Ctrl3/VM2			
4. Check vhost or VMs did not crash			
5. Run fio integrity on both devices			
6. Reboot VMs			
7. Run fio integrity again on all devices			


### Hotdetach Test Cases prerequisites
1. Get latest SPDK.
2. run "git submodule update --init" in SPDK directory.
3. Run vhost with 4 controllers, each with 2 devices. If dont'have 8 disks use split.
4. Run vms, first with ctrlr-1 and ctrlr-2 and second with ctrlr-3 and ctrlr-4

## Test Case 1
1. Detatch NVMe from Ctrl1 during fio				
2. Check vhost or VMs did not crash				
3. Check that detatched device is gone from VM				
4. Reboot VMs				
5. Check that detatched device is gone from VM				
6. Check that all other devices are in place				
7. Run fio integrity on all remianing devices				

## Test Case 2
1. Run fio on 1 device from Ctrl 1					
2. Detatch NVMe from Ctrl1 during fio traffic					
3. Check vhost or VMs did not crash					
4. Check that fio job run  on detached device stopped and failed					
5. Check that detatched device is gone from VM					
6. Reboot VMs					
7. Check that detatched device is gone from VM					
8. Run fio integrity on all remianing devices					

## Test Case 3
1. Run fio with integrity on all devices, except one						
2. Detatch NVMe without traffic during fio running on other devices						
3. Check vhost or VMs did not crash						
4. Check that fio jobs did not fail						
5. Check that detatched device is gone from VM						
6. Reboot VMs						
7. Check that detatched device is gone from VM						
8. Check that all other devices are in place						

## Test Case 4
1. Run fio on 1 device from Ctrl 1					
2. Run separate fio with integrity on all other devices (all VMs)					
3. Detatch NVMe from Ctrl1 during fio traffic					
4. Check vhost or VMs did not crash					
5. Check that fio job run  on detached device stopped and failed					
6. Check that other fio jobs did not fail					
7. Check that detatched device is gone from VM					
8. Reboot VMs					
9. Check that detatched device is gone from VM					
10. Check that all other devices are in place					
11. Run fio integrity on all remianing devices		
