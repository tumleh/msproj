%Load_Grapher.m
%Plots delays experienced by different loads
clear all;
close all;
file_name = ('flow');%char('ideal_qcsma','slotted_qcsma','slip');
first_file = 1;%first file
last_file = 1;%last file
ideal_qcsma = zeros(1,last_file-first_file+1);
slotted_qcsma = zeros(1,last_file-first_file+1);
slip = zeros(1,last_file-first_file+1);
load = zeros(1,last_file-first_file+1);
for sim_type = 1:1
    for file = first_file:last_file
        input = csvread(strcat('./output/',file_name(sim_type,:),int2str(file),'.csv'));%'logs/sim.csv');
        index = 1-first_file+file;
%         label = ['flow queue','switch queue','packet delays']
        [w,h] = size(input);
        %imagesc(input.*(input<90).*(input>=0));colorbar;

        num_flows=input(1,1);
        row = input(1,2);
        
        flow_src = input(2,1:num_flows);
        figure;stem(flow_src);
        flow_dest = input(3,1:num_flows);
        figure;stem(flow_dest);
        off_2_on = input(4,1:num_flows);
        figure;stem(off_2_on);
        on_2_off = input(5,1:num_flows);
        figure;stem(on_2_off);
        flow_gen_rate = input(6,1:num_flows);
        figure;stem(flow_gen_rate);
        
        type_matrix = zeros(row,row);
        for f = 1:num_flows
            type = 2;
            if(on_2_off(f)==.3)
                type = 1;
            end
            type_matrix(flow_src(f)+1,flow_dest(f)+1)=type;
        end
        
        figure;imagesc(type_matrix);colorbar;
        return;
        for i= 1:length(dim_loc)
                    temp_index = dim_loc(i);
                    data_matrix = input(temp_index+1:+temp_index+input(temp_index,1),1:input(temp_index,2));
                    data=zeros(1,3);
                    [a,b]=size(data_matrix);
                    data(1) = min(min(data_matrix));
                    data(3) = max(max(data_matrix));
                    data(2) = sum(sum(data_matrix))/(a*b);
                    switch(i)
                        case 1 %tells us the load and avg_pkt_length
                            load(index) = data_matrix(1);
                            avg_pkt_length = data_matrix(2);
                        case (7+1)% average delay
                            switch(sim_type)
                                case 1
                                    ideal_qcsma(index) = data(2)/avg_pkt_length;
                                case 2
                                    slotted_qcsma(index) = data(2)/avg_pkt_length;
                                case 3
                                    slip(index) = data(2)/avg_pkt_length;
                            end
                    end
    %                 set(errorbar([data(2)],[0],'r'),'linestyle','none','LineWidth',3)
    %                 hold;
    %                 set(errorbar([mean([data(1),data(3)])],[((data(3)-data(1))/2)],'b'),'linestyle','none','LineWidth',3)
    %                 set(gca,'xtick',[1],'xticklabel',{'label appropriately'})
    %                 hold;
    %                 figure;
    %                 stem(data);%imagesc(data_matrix);colorbar;
    %                 name='';
%                     switch(mod(i,3))
%                         case 0
%                             name = 'max';%title(strcat('max',label(i/1+1)));
%                         case 1
%                             name ='average';%title(strcat('average',label(i/1+1)));
%                         case 2
%                             name = 'variance';%title(strcat(label(i/1+1),'variance'));
%                     end
%                     switch(ceil(i/3))
%                         case 1
%                             name = strcat(name,' flow queue');%'max';%title(strcat('max',label(i/1+1)));
%                         case 2
%                             name =strcat(name,' switch queue');%title(strcat('average',label(i/1+1)));
%                         case 3
%                             name = strcat(name,' packet delay');%title(strcat(label(i/1+1),'variance'));
%                     end
                    %title(name);
        end
    end
end
figure;
plot(load,ideal_qcsma,'k',load,slotted_qcsma,'r',load,slip,'g');
legend('ideal qcsma','time slotted qcsma','slip');
xlabel('load');
ylabel('delay in avg pkt lengths');
title('iid load vs delay');

% figure;
% plot(load,log(ideal_qcsma),'k',load,log(slotted_qcsma),'r',load,log(slip),'g');
% legend('ideal qcsma','time slotted qcsma','slip');
% xlabel('load');
% ylabel('delay in avg pkt lengths');
% title('iid load vs log delay');